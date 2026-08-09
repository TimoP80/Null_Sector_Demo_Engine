// ---------------------------------------------------------------------------
// perftimer.hpp - reusable GPU-frame timer for effects. Bracket a frame's
// passes with beginFrame()/endFrame() and the timer reports EMA / median /
// mean ms per frame, drives a periodic "X.XX ms/frame GPU" log line, and
// offers a profiling window for tools (--check-shadertoy).
//
// Used by ShadertoyFX, SceneFX/ParticleStormFX and the post stack, so every
// effect can answer "what is this costing?" - and what a renderScale option
// is worth - without each consumer reimplementing the GL plumbing.
//
//   GL_TIMESTAMP (not GL_TIME_ELAPSED) so several timers can be active in
//   one frame - GL allows only one active elapsed-time query per context.
//   The measured time covers exactly the commands between beginFrame() and
//   endFrame() (the effect's passes, not the whole frame).
//
//   A ring of kSlots query pairs is used because the GPU completes them
//   asynchronously: in a vsync'd/unthrottled loop the CPU can run AHEAD of
//   the GPU, and a single next-frame availability check would find the
//   previous pair unfinished and drop nearly every sample. Each pair stays
//   pending and is retried every frame until the GPU finishes it (see
//   PerfRingState), so samples flow at any frame pacing.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/gl.hpp"
#include "engine/gputimer.hpp"

#include <string>
#include <utility>

namespace ns {

class PerfTimer {
public:
  PerfTimer() = default;
  ~PerfTimer() {
    for (auto& s : slots_) {
      if (s.start) { ::glDeleteQueries(1, &s.start); s.start = 0; }
      if (s.end) { ::glDeleteQueries(1, &s.end); s.end = 0; }
    }
  }
  PerfTimer(const PerfTimer&) = delete;
  PerfTimer& operator=(const PerfTimer&) = delete;

  /** stamp the start of this frame's work + fold in any samples the GPU has
   *  finished since the last frame. Call right before your passes, every
   *  frame (queries are created lazily on first use, so the timer can be
   *  constructed before the GL context exists). */
  void beginFrame() {
    if (inFrame_) return;  // a stray double-begin can never merge two frames
    ensureQueries();
    // retry the OLDEST uncollected pair; if the GPU has finished it, fold it
    // into the stats. Unready pairs stay pending and are retried next frame.
    if (ring_.pending > 0) {
      const int oldest = ring_.oldest();
      GLint avail = 0;
      ::glGetQueryObjectiv(slots_[oldest].end, ::gl::QUERY_RESULT_AVAILABLE, &avail);
      if (avail) {
        GLuint64 endNs = 0, startNs = 0;
        ::glGetQueryObjectui64v(slots_[oldest].end, ::gl::QUERY_RESULT, &endNs);
        ::glGetQueryObjectui64v(slots_[oldest].start, ::gl::QUERY_RESULT, &startNs);
        ring_.collected();
        if (endNs > startNs) {  // skip zero-length pairs
          sampleThisFrame_ = true;
          stats_.add((float)((double)(endNs - startNs) / 1e6));
        }
      }
    }
    ::glQueryCounter(slots_[ring_.write].start, ::gl::TIMESTAMP);
    inFrame_ = true;
  }

  /** stamp the end of this frame's work. Call right after your passes. */
  void endFrame() {
    if (!inFrame_) return;
    ::glQueryCounter(slots_[ring_.write].end, ::gl::TIMESTAMP);
    ring_.issued();  // when full, the write slot overwrote the oldest (drop)
    inFrame_ = false;
  }

  // --- stats -----------------------------------------------------------------
  /** current EMA in ms (0 until the first sample - tools can skip effects
   *  that have never collected a sample, e.g. the per-second --perf-csv
   *  rows). */
  double emaMs() const { return stats_.ema(); }

  /** the most recent RAW per-frame sample in ms (0 until the first one) -
   *  unsmoothed, so the --perf-raw dump can plot the spikes the EMA hides.
   *  Only updates on frames where the GPU actually finished a sample. */
  double lastRawMs() const { return stats_.lastMs(); }
  /** mean ms/frame over the last profiling window (-1 with no samples). */
  double meanMs() const { return stats_.meanMs(); }
  /** median ms/frame over the last profiling window (-1 with no samples). */
  double medianMs() const { return stats_.medianMs(); }

  /** stable snapshot over the recorded run window (the --perf-json dump:
   *  median is robust to spikes, min/max bound the spread, frames is the
   *  total samples collected, not just the window). */
  PerfSample sample() const {
    PerfSample s;
    s.medianMs = stats_.medianRecorded();
    s.meanMs = stats_.meanRecorded();
    s.minMs = stats_.minRecorded();
    s.maxMs = stats_.maxRecorded();
    s.frames = stats_.recordedCount();
    return s;
  }

  // --- periodic log ------------------------------------------------------------
  /** true once per logEvery COLLECTED samples, only on frames that collected
   *  one (a driver without timestamp queries never logs). Call right after
   *  endFrame() every frame and log logLine() when it returns true. */
  bool logDue() {
    const bool due = sampleThisFrame_ && stats_.logDue();
    sampleThisFrame_ = false;
    return due;
  }

  /** "<label>: 1.23 ms/frame GPU" - the label part is dropped when empty, so
   *  consumers append their own context, e.g. "(renderScale 0.50, buffers
   *  800x450)". */
  std::string logLine() const {
    std::string line = label_.empty() ? "" : label_ + ": ";
    return line + fmtMs(stats_.emaMs) + " ms/frame GPU";
  }

  void setLabel(std::string l) { label_ = std::move(l); }

  // --- profiling window (tools path: --check-shadertoy) -------------------------
  /** start a window; the mean/median over the next collected samples are
   *  read after endProfile(). */
  void beginProfile() { stats_.beginProfile(); }

  /** blocking-drain the pending pairs so the last measured frames count,
   *  then stop the window. Tools path only - never in the render loop. */
  void endProfile() {
    drain();
    stats_.endProfile();
  }

  /** blocking-drain EVERY pending pair (oldest first). Tools path only:
   *  lets a tool discard unmeasured warmup frames BEFORE beginProfile(), so
   *  the window starts empty. On a driver without timestamp queries the
   *  results read back 0 and the zero-length guard keeps them out. */
  void drain() {
    while (ring_.pending > 0) {
      const int oldest = ring_.oldest();
      GLuint64 endNs = 0, startNs = 0;
      ::glGetQueryObjectui64v(slots_[oldest].end, ::gl::QUERY_RESULT, &endNs);
      ::glGetQueryObjectui64v(slots_[oldest].start, ::gl::QUERY_RESULT, &startNs);
      ring_.collected();
      if (endNs > startNs) stats_.add((float)((double)(endNs - startNs) / 1e6));
    }
  }

private:
  static constexpr int kSlots = 16;
  struct Slot {
    unsigned start = 0, end = 0;
  };
  Slot slots_[kSlots];
  PerfRingState ring_{kSlots};
  GpuTimeStats stats_;
  std::string label_;
  bool inFrame_ = false;
  bool sampleThisFrame_ = false;

  void ensureQueries() {
    if (slots_[0].start) return;
    for (auto& s : slots_) {
      ::glGenQueries(1, &s.start);
      ::glGenQueries(1, &s.end);
    }
    // warm one pair with a blocking read - some drivers return garbage for
    // the first timestamp reads, and it leaves the ids in a completed state
    ::glQueryCounter(slots_[0].start, ::gl::TIMESTAMP);
    ::glQueryCounter(slots_[0].end, ::gl::TIMESTAMP);
    GLuint64 w0 = 0, w1 = 0;
    ::glGetQueryObjectui64v(slots_[0].start, ::gl::QUERY_RESULT, &w0);
    ::glGetQueryObjectui64v(slots_[0].end, ::gl::QUERY_RESULT, &w1);
    ring_ = PerfRingState(kSlots);
  }
};

}  // namespace ns
