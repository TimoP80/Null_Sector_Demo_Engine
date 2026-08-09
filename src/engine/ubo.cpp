#include "engine/ubo.hpp"
#include "effects/base_fwd.hpp"
#include "engine/audio.hpp"
#include "engine/camera.hpp"
#include "engine/renderer.hpp"
#include "engine/timeline.hpp"
#include <algorithm>
#include <cmath>

namespace ns {

void SharedBlock::write(const EffectContext* ctx) {
  TimelineState& tl = ctx->timeline->s;
  const Camera& cam = *ctx->camera;
  const React& re = ctx->audio->react;
  const Renderer& r = *ctx->r;

  // a change in time means a new frame -> exactly one upload per frame
  if (data[OFF_UTIME] != ctx->time) dirty = true;

  data[OFF_URES] = (float)r.resW;
  data[OFF_URES + 1] = (float)r.resH;
  data[OFF_UTIME] = ctx->time;
  data[OFF_UFOVTAN] = std::tan(cam.fov / 2.0f);

  data[OFF_UCAMPOS + 0] = cam.pos[0];
  data[OFF_UCAMPOS + 1] = cam.pos[1];
  data[OFF_UCAMPOS + 2] = cam.pos[2];

  // basis matrix (column-major 3x3) -> std140 mat3 columns
  const float cols[3][3] = {
    {cam.right[0], cam.right[1], cam.right[2]},
    {cam.up[0], cam.up[1], cam.up[2]},
    {-cam.fwd[0], -cam.fwd[1], -cam.fwd[2]},
  };
  for (int c = 0; c < 3; c++) {
    data[OFF_UCAMROT + c * 4 + 0] = cols[c][0];
    data[OFF_UCAMROT + c * 4 + 1] = cols[c][1];
    data[OFF_UCAMROT + c * 4 + 2] = cols[c][2];
  }
  for (int i = 0; i < 16; i++) data[OFF_UVIEW + i] = cam.view[i];
  for (int i = 0; i < 16; i++) data[OFF_UPROJ + i] = cam.proj[i];

  data[OFF_UBEAT] = tl.beat;
  data[OFF_UPULSE] = tl.beatPulse;
  data[OFF_UINTENSITY] = tl.intensity;
  data[OFF_USECTIONLOCAL] = tl.sectionLocal;
  // musical timing anchors: shaders normalize section progress and bar-based
  // cycles against these instead of hardcoded seconds, so re-times stay in sync
  data[OFF_USECTIONDUR] = tl.section.duration;
  data[OFF_USECBAR] = BAR;
  data[OFF_UBASS] = re.bass.load();
  data[OFF_UONSET] = re.onset.load();
  data[OFF_UANTICIPATION] = tl.anticipation;
  data[OFF_UEXITRAMP] = tl.exitRamp;

  // musical chord progression (bar-quantized hue states)
  data[OFF_UMUSICHUE] = tl.musicHue;
  data[OFF_UMUSICHUE2] = tl.musicHue2;
  data[OFF_UBARPHASE] = tl.barPhase;
  data[OFF_UBAR] = tl.bar;

  // --- kick assembly ratchet --------------------------------------------------
  // One-way accumulator per kick hit: each real kick permanently snaps the
  // cathedral's pillars/arches forward, and in the Infinite Machine it revs
  // up the gears and tightens the ring precession. Assembly is computed from
  // the audio analyser kick (not the beat grid), so it fires on real bass
  // transients irrespective of tempo.
  //
  // The branch is keyed on section NAME so the reprise scenes reuse it:
  //   cathedral / infinitemachine  kick-increment + slow trickle (assemble/rev)
  //   infinitemachine entry        rev from COLD (each machine section spins
  //                                up from 0 - section 4 AND the reascension
  //                                reprise both get a visible rev-up)
  //   deconstruction               the ghost resurrects the cathedral to full
  //                                on entry, then it decays as it tears down
  //   section 8 (reprise)          decay (the quantum tunnel renders it)
  //   everything else              hold
  //
  // ~0.055 per kick = ~18 kicks to reach 1.0 (the cathedral section is 9 bars
  // at 216 BPM = ~18 downbeats). We use a local static edge detector that
  // persists across frames so the accumulator is sticky.
  {
    static float s_prevKick = 0.0f;
    static float s_gate = 0.0f;    // min-interval gate (same 0.12s as KickFlash)
    static std::string s_prevName; // section-entry edge (per name, so reprises re-fire)
    const float kick = re.kick.load();
    const float dKick = kick - s_prevKick;
    s_prevKick = kick;
    s_gate = std::max(0.0f, s_gate - ctx->dt);

    float asm_ = tl.assembly;
    const std::string& n = tl.section.name;
    const bool entered = (s_prevName != n);

    if (n == "infinitemachine") {
      // the machine must rev up from cold each time it appears: the cathedral
      // saturates the accumulator to ~1.0, so zero it on entry so the
      // section's kicks visibly spin it up again from 0 (10 bars = ~20
      // downbeats to refill)
      if (entered) asm_ = 0.0f;
      if (s_gate <= 0.0f && dKick > 0.04f && kick > 0.10f) {
        asm_ = std::min(1.0f, asm_ + 0.055f);
        s_gate = 0.12f;
      }
      asm_ = std::min(1.0f, asm_ + ctx->dt * 0.008f);
    } else if (n == "cathedral") {
      if (s_gate <= 0.0f && dKick > 0.04f && kick > 0.10f) {
        asm_ = std::min(1.0f, asm_ + 0.055f);
        s_gate = 0.12f;
      }
      asm_ = std::min(1.0f, asm_ + ctx->dt * 0.008f);
    } else if (n == "deconstruction") {
      // the ghost resurrects the cathedral to full, then tears it down - the
      // collapse is spread across the 28-bar breakdown (buildCur's sqrt keeps
      // it standing through the early decay, then it snaps)
      if (entered) asm_ = 1.0f;
      asm_ = std::max(0.0f, asm_ - ctx->dt * 0.06f);
    } else if (n == "reprise") {
      // reprise: the cathedral deconstructs — decay toward 0
      asm_ = std::max(0.0f, asm_ - ctx->dt * 0.15f);
    }

    data[OFF_UASSEMBLY] = asm_;
    tl.assembly = asm_;  // persist across frames
    s_prevName = n;
  }
}

}  // namespace ns