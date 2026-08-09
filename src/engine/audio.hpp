// ---------------------------------------------------------------------------
// Audio engine (port of src/engine/audio.ts).
// Plays an external WAV/MP3 track (audio.wav / audio.mp3 by default - searched
// in assets, the exe dir, then cwd; overridable with --track=, suppressed with
// --no-track). MP3 comes from miniaudio's bundled dr_mp3 (the small, lossy
// format preferred for distribution). There is no built-in synthesizer any
// more: without a track the show runs silent (the wall-clock fallback still
// advances the show clock so the timeline never freezes). The analyser stays
// in the chain either way so visual reactivity (react) follows the real audio.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MA_NO_FLAC

struct ma_device;  // fwd decl (defined by miniaudio.h, included only in audio.cpp)

namespace ns {

struct React {
  // written on the audio thread, read on the main thread (atomics)
  std::atomic<float> bass{0}, mid{0}, treble{0}, energy{0};
  std::atomic<float> rms{0}, centroid{0.35f}, onset{0}, kick{0}, peak{0};
};

class AudioEngine {
public:
  AudioEngine() = default;
  ~AudioEngine();
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  /** lifecycle of a background decode started by beginAsyncSwap */
  enum class AsyncState { Idle, Decoding, Ready, Failed };

  /** ready the device (no audio until start()) */
  bool init();

  /** load an external WAV/MP3 track (the only audio source; without one the
   *  show runs silent) */
  void loadTrack(const std::string& path);

  /** swap the external track at runtime (the demo editor's audio control):
   *  stops the device so the audio thread can't race the decode, decodes the
   *  new file, syncs the playhead to seekSec (0 = from the start), and
   *  restarts. An empty path stops audio (silence). Returns false if a
   *  non-empty path failed to decode (the previous source is kept).
   *  now() is held for the duration of the swap. Safe any time after init().
   *  This is the synchronous wrapper - it starts an async decode and waits
   *  for it, so a large file blocks the caller. The editor uses the async
   *  trio below instead to keep the UI responsive. */
  bool swapTrack(const std::string& path, float seekSec = 0);

  /** async swap (the editor's non-blocking path): kicks off a background
   *  decode of path (empty = stop audio). The current source keeps playing
   *  while the worker decodes; poll asyncStatus(); when it returns Ready call
   *  applyAsyncSwap() (main thread) to commit the swap with a single device
   *  stop/start. A previously pending swap is superseded. */
  void beginAsyncSwap(const std::string& path, float seekSec = 0);
  /** Idle = nothing pending, Decoding = worker running (spinner), Ready =
   *  decoded and waiting for applyAsyncSwap(), Failed = decode failed and
   *  the previous source was kept. */
  AsyncState asyncStatus() const;
  /** path being decoded (empty while Idle / Failed) */
  std::string asyncPath() const;
  /** commit a Ready async swap (no-op otherwise); returns swapTrack()'s
   *  result semantics and resets the pending state. */
  bool applyAsyncSwap();

  /** seek the external track's playhead (device stopped briefly); no-op when
   *  no track is loaded. The show clock (now()) is unaffected, so after a
   *  timeline scrub the music re-syncs to the show position and both advance
   *  together. */
  void seekTrack(float sec);

  /** the path of the loaded track (empty when none is loaded) */
  const std::string& trackPath() const { return trackPath_; }

  /** read-only decoded track samples for the editor's waveform strip: stereo
   *  interleaved float, resampled to sampleRate(). Empty when no track is
   *  loaded. Only read on the main thread (never from the audio callback). */
  const std::vector<float>& trackSamples() const { return trackData_; }
  /** total decoded stereo frames (trackSamples().size() / 2) */
  uint64_t trackFrames() const { return trackFrames_; }
  /** engine sample rate (48000) */
  unsigned sampleRate() const { return sampleRate_; }

  // --- live FFT spectrogram (rolling spectrum ring) ---------------------------
  // The audio callback captures the mixed output into a ring of FFT columns
  // (log-spaced magnitude bins, dB-compressed); the editor renders it as a
  // column-scrolling heat strip under the waveform. The writer (audio thread)
  // publishes each column fully before incrementing the monotonic count, so a
  // single-consumer reader (the editor's per-frame snapshot) never sees a
  // torn column; the ring wrap is far older than the visible history.
  static constexpr unsigned kSpecN = 1024;    // FFT window (frames ~21ms)
  static constexpr unsigned kSpecHop = 512;   // column cadence (frames ~11ms)
  static constexpr unsigned kSpecBins = 40;   // log-spaced magnitude bins
  static constexpr uint32_t kSpecCap = 720;   // ring depth (~7.7 s of columns)
  unsigned spectrumBins() const { return kSpecBins; }
  uint32_t spectrumCap() const { return kSpecCap; }
  uint32_t spectrumCount() const { return specCount_.load(std::memory_order_acquire); }
  /** the produced column `col` (< spectrumCount()): kSpecBins floats 0..1 */
  const float* spectrumColumn(uint32_t col) const {
    return spec_.data() + (size_t)(col % kSpecCap) * kSpecBins;
  }
  /** show-clock time (seconds) the column was captured at */
  float spectrumColumnTime(uint32_t col) const { return specT_[col % kSpecCap]; }

  /** begin playback: open the device when a track is loaded; without one the
   *  show runs silent (the wall-clock fallback keeps the clock moving) */
  void start();

  /** seconds since playback started (the show clock) */
  float now() const;

  /** per-frame analyser poll (reads the atomics) */
  void update();

  /** headless playback self-test: feeds audioCallback one block and asserts
   *  non-silence + show-clock advance + analyser energy (for --smoke-audio) */
  bool selfTest();

  bool started = false;
  bool trackMode = false;
  float trackDuration = 0;
  React react;

private:
  friend void audioCallback(void* userdata, float* out, unsigned frames);

  bool startDevice();           // open + start the real playback device
  void prepareAnalyser();       // analyser bands (no synth buffers anymore)

  struct Decoded {
    bool ok = false;
    std::vector<float> data;  // stereo f32 at sampleRate_
    uint64_t frames = 0;
  };
  // decode path off the main thread (both swapTrack and the async worker use
  // it); cancel lets a torn-down engine stop a long decode early
  Decoded decodeFile(const std::string& path, std::atomic<bool>& cancel);
  void applyReady(const std::string& path, float seekSec);  // commit a Ready swap
  /** per-block band analysis + show-clock advance, fed by the audio callback
   *  (or the main thread's wall-clock fallback - never concurrently) */
  void analyzeAndAdvance(float* out, unsigned frames);

  // analyser state (bands + envelope followers). Updated on the audio thread
  // or the main thread's wall-clock fallback - never concurrently.
  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    void lowpass(float f, float q, float sr) {
      const float w0 = 6.2831853f * f / sr;
      const float alpha = std::sin(w0) / (2 * q);
      const float cw = std::cos(w0);
      const float a0 = 1 + alpha;
      b0 = (1 - cw) / 2 / a0; b1 = (1 - cw) / a0; b2 = (1 - cw) / 2 / a0;
      a1 = -2 * cw / a0; a2 = (1 - alpha) / a0;
    }
    void bandpass(float f, float q, float sr) {
      const float w0 = 6.2831853f * f / sr;
      const float alpha = std::sin(w0) / (2 * q);
      const float cw = std::cos(w0);
      const float a0 = 1 + alpha;
      b0 = alpha / a0; b1 = 0; b2 = -alpha / a0;
      a1 = -2 * cw / a0; a2 = (1 - alpha) / a0;
    }
    void highpass(float f, float q, float sr) {
      const float w0 = 6.2831853f * f / sr;
      const float alpha = std::sin(w0) / (2 * q);
      const float cw = std::cos(w0);
      const float a0 = 1 + alpha;
      b0 = (1 + cw) / 2 / a0; b1 = -(1 + cw) / a0; b2 = (1 + cw) / 2 / a0;
      a1 = -2 * cw / a0; a2 = (1 - alpha) / a0;
    }
    float process(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  struct Analyser {
    Biquad bandBass, bandMid, bandTre;
    float envEnergy = 0, envBass = 0;
    float smRms = 0, smCentroid = 0.35f, smOnset = 0, smKick = 0;
    uint64_t frame = 0;  // total frames rendered (advances the show clock)
  };
  Analyser analyser_;

  // state shared with the callback
  // async decode worker state (worker thread writes under publishMtx_, the
  // main thread reads under it in applyAsyncSwap). pendingPath_ is also
  // written by beginAsyncSwap on the main thread while the worker reads it -
  // so both sides copy under the same mutex; the string lives for the whole
  // decode so a read race on its contents would be benign anyway.
  mutable std::mutex publishMtx_;
  AsyncState asyncState_ = AsyncState::Idle;
  std::string pendingPath_;   // path being decoded (or dropped) right now
  std::vector<float> pendingData_;  // decoded stereo f32 (Ready only)
  uint64_t pendingFrames_ = 0;
  float pendingSeekSec_ = 0;
  std::atomic<bool> cancelWorker_{false};  // set at teardown / supersede so a
                                           // worker mid-decode exits promptly
  uint64_t asyncGen_ = 0;  // generation of the current async swap; a worker
                           // whose generation is stale was superseded and its
                           // publish is discarded (no bogus 'failed' log)
  std::thread worker_;

  bool inited_ = false;
  std::atomic<double> frameCursor_{0};  // frames rendered (audio thread writes, now() reads)
  unsigned sampleRate_ = 48000;
  bool primed_ = false;
  double lastAudioNow_ = 0;
  double lastWall_ = 0;      // wall clock of the last update() (wall-clock fallback only)
  ma_device* device_ = nullptr;   // live miniaudio playback device (opened at start)
  bool deviceLive_ = false;       // device streaming; update() becomes a no-op
  double lastCursor_ = 0;         // stall-guard: last frameCursor_ seen on the main thread
  int stallFrames_ = 0;           // stall-guard: consecutive frames with no clock advance

  std::string trackPath_;

  // decoded external track (via miniaudio's bundled dr_wav/dr_mp3, resampled
  // to sampleRate_ stereo float at load time). When trackMode is set the
  // audio callback copies these samples verbatim; otherwise it outputs
  // silence (there is no built-in synth anymore).
  std::vector<float> trackData_;
  std::atomic<uint64_t> trackPos_{0};  // frames consumed so far (atomic: the
                                        // callback re-reads it per sample, and
                                        // seekTrack writes it without stopping
                                        // the device so scrubbing stays clean)
  uint64_t trackFrames_ = 0;  // total stereo frames
  unsigned trackChannels_ = 2;

  // spectrum ring state: the rolling mono window + FFT scratch are only
  // touched by the audio callback (or the main thread's wall-clock fallback -
  // never concurrently), so they need no locking; only the publish is shared.
  std::vector<float> specIn_;      // rolling mono window (kSpecN)
  uint32_t specInPos_ = 0;
  uint64_t specFilled_ = 0;        // mono frames fed so far
  std::vector<float> spec_;        // kSpecCap * kSpecBins
  std::vector<float> specT_;       // capture time per column (seconds)
  std::atomic<uint32_t> specCount_{0};

public:
  // capture hook: called by the audio callback (via analyzeAndAdvance) with
  // each mixed output block; the FFT + ring publish happen here
  void captureSpectrum(const float* out, unsigned frames);

private:
  void emitSpectrumColumn();  // FFT + publish one ring column
};

}  // namespace ns
