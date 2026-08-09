// ---------------------------------------------------------------------------
// gputimer.hpp - GL-free GPU-time statistics + timestamp-pair ring
// bookkeeping for effects that measure their own cost with GL_TIMESTAMP
// timer queries (the GL side lives in PerfTimer, see perftimer.hpp). Only
// the accumulation math and ring index tracking live here, so the whole
// header is unit-testable without a GL context.
//
//   GpuTimeStats   EMA of ms/frame (for the periodic "X ms/frame GPU" log),
//                  a beginProfile()/endProfile() window for tools like
//                  --check-shadertoy to measure a mean/median over N frames,
//                  and a log throttle that fires once per logEvery samples.
//   PerfRingState  which ring slot the current frame writes + which issued
//                  query pairs are still uncollected (the tricky part of the
//                  ring, kept GL-free so it is testable).
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ns {

struct GpuTimeStats {
  // EMA of the per-frame GPU time in ms (0 until the first sample).
  double emaMs = 0.0;

  // periodic-log throttle: fires once per this many collected samples
  int logEvery = 120;

  // profiling window (beginProfile/endProfile)
  bool profiling = false;
  double profileSumMs = 0.0;
  int profileFrames = 0;
  std::vector<float> profileSamples;  // per-frame ms, only while profiling

  // per-run recorder (--perf-json dumps): circular buffer of the last
  // recordCap per-frame ms samples + a running total. Always on, so the exit
  // dump can report a stable median over the run without unbounded growth.
  // recordCap is const: fixed at construction (changing it after the buffer
  // filled would corrupt the circular-window indexing).
  const int recordCap = 1024;
  explicit GpuTimeStats(int cap = 1024) : recordCap(cap) {}
  std::vector<float> rec;  // circular, at most recordCap entries
  size_t recCount = 0;     // samples ever recorded (includes overwritten ones)

  /** feed one measured per-frame sample (ms). */
  void add(float ms) {
    lastMs_ = ms;
    emaMs = emaMs <= 0.0 ? (double)ms : emaMs * 0.9 + (double)ms * 0.1;
    if (profiling) {
      profileSumMs += ms;
      profileFrames++;
      profileSamples.push_back(ms);
    }
    record(ms);
  }

  void record(float ms) {
    recCount++;
    if (recordCap <= 0) return;
    if (rec.size() < (size_t)recordCap) rec.push_back(ms);
    else rec[(recCount - 1) % (size_t)recordCap] = ms;
  }

  /** total samples ever recorded (may exceed the circular window). */
  size_t recordedCount() const { return recCount; }

  /** median ms/frame over the recorded window (-1 when nothing recorded). */
  double medianRecorded() const {
    if (rec.empty()) return -1.0;
    std::vector<float> v = rec;
    std::sort(v.begin(), v.end());
    return (double)v[v.size() / 2];
  }

  double meanRecorded() const {
    if (rec.empty()) return -1.0;
    double s = 0.0;
    for (float m : rec) s += m;
    return s / (double)rec.size();
  }

  double minRecorded() const {
    if (rec.empty()) return -1.0;
    double m = rec[0];
    for (float v : rec) m = std::min(m, (double)v);
    return m;
  }

  double maxRecorded() const {
    if (rec.empty()) return -1.0;
    double m = rec[0];
    for (float v : rec) m = std::max(m, (double)v);
    return m;
  }

  /** start collecting a mean/median over the next add() samples. */
  void beginProfile() {
    profiling = true;
    profileSumMs = 0.0;
    profileFrames = 0;
    profileSamples.clear();
  }

  /** stop the window; the mean and median over it are read via meanMs() /
   *  medianMs() (-1 when no samples came in). */
  void endProfile() {
    profiling = false;
  }

  double meanMs() const {
    return profileFrames > 0 ? profileSumMs / (double)profileFrames : -1.0;
  }

  /** median ms/frame over the window - robust against a single outlier frame
   *  (a driver hiccup or the first touch of freshly allocated render targets
   *  can inflate the mean by 2-3x over a short window; the median ignores
   *  it). Returns the upper median for an even sample count. */
  double medianMs() const {
    if (profileSamples.empty()) return -1.0;
    std::vector<float> v = profileSamples;
    std::sort(v.begin(), v.end());
    return (double)v[v.size() / 2];
  }

  /** call after each collected sample; true once per logEvery samples. */
  bool logDue() {
    logEvery = logEvery > 0 ? logEvery : 1;
    if (++samplesSinceLog >= logEvery) {
      samplesSinceLog = 0;
      return true;
    }
    return false;
  }

  /** current EMA in ms (0 until the first sample - a timed effect that has
   *  never collected a sample reports 0, so tools can skip it). */
  double ema() const { return emaMs; }

  /** the most recent RAW per-frame sample in ms (0 until the first one) -
   *  the unsmoothed value, so tools like the --perf-raw dump can show
   *  spikes that the EMA hides. */
  double lastMs() const { return lastMs_; }

private:
  int samplesSinceLog = 0;
  double lastMs_ = 0.0;  // most recent raw sample fed to add()
};

/** one effect's stable GPU-time snapshot over its recorded window
 *  (--perf-json dumps). -1 fields mean nothing was recorded. */
struct PerfSample {
  double medianMs = -1.0;
  double meanMs = -1.0;
  double minMs = -1.0;
  double maxMs = -1.0;
  size_t frames = 0;  // total samples collected (may exceed the window)
};

/** GL-free bookkeeping for PerfTimer's ring of GL_TIMESTAMP pairs: which
 *  slot the current frame writes and how many issued pairs are still
 *  uncollected. The OLDEST uncollected pair is retried every frame until the
 *  GPU finishes it (a single next-frame availability check would drop nearly
 *  every sample when the CPU runs ahead of the GPU). When the ring is full
 *  the write slot IS the oldest, so issuing overwrites (drops) it - the GPU
 *  never finished that sample anyway. */
struct PerfRingState {
  int slots = 0;
  int write = 0;     // slot for the CURRENT frame's pair
  int pending = 0;   // issued pairs not yet collected

  explicit PerfRingState(int n) : slots(n) {}

  /** index of the oldest uncollected pair. */
  int oldest() const { return (write - pending + slots) % slots; }

  /** a frame just issued its pair (write advances; when the ring is full the
   *  write slot overwrote the oldest pair - the drop). */
  void issued() {
    write = (write + 1) % slots;
    if (pending < slots) pending++;
  }

  /** the oldest pair was collected. */
  void collected() {
    if (pending > 0) pending--;
  }
};

/** format a ms value as "1.23" for log lines. */
inline std::string fmtMs(double ms) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.2f", ms);
  return b;
}

}  // namespace ns
