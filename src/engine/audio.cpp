#include "engine/audio.hpp"
#include "framework/vfs/vfs.hpp"
#include "engine/schedule.hpp"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#ifndef NOMINMAX
#define NOMINMAX  // windows.h min/max macros would clobber std::min/std::max
#endif
#include "miniaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <iterator>
#include <fstream>
#include <filesystem>
#include <new>
#include <thread>
#include <vector>

namespace ns {

// miniaudio's dr_wav/dr_mp3 decoder is NOT safe when two decoder instances
// are used concurrently (a background decode racing a sync one wedged the
// worker). All decode calls - sync loadTrack, swapTrack and the async worker
// - go through this one mutex, so only one decoder is ever live at a time.
static std::mutex gDecodeMtx;

/** per-block band analysis + show-clock advance. Fed by the audio callback
 *  (audio thread) or the main thread's wall-clock fallback - never
 *  concurrently, so the filter state below needs no locking. */
void AudioEngine::analyzeAndAdvance(float* out, unsigned frames) {
  // simple: energy = mean |x| of the block; bands via the filters above,
  // computed on the last 256-sample window each frame
  Analyser& S = analyser_;
  float bassL = 0, midL = 0, treL = 0, enL = 0, peakL = 0, rmsL = 0;
  const int N = 256;
  const int startF = (int)((frames > N) ? (frames - N) : 0);
  for (int i = startF; i < (int)frames; i++) {
    const float x = (out[i * 2] + out[i * 2 + 1]) * 0.5f;
    // approximate bands: re-filter the block through the followers
    const float b = S.bandBass.process(x);
    const float m = S.bandMid.process(x);
    const float t = S.bandTre.process(x);
    bassL += b * b; midL += m * m; treL += t * t;
    enL += std::abs(x); rmsL += x * x;
    peakL = std::max(peakL, std::abs(x));
  }
  const int cnt = frames > 0 ? (int)frames - startF : 1;
  bassL = std::sqrt(bassL / cnt);
  midL = std::sqrt(midL / cnt);
  treL = std::sqrt(treL / cnt);
  enL = enL / cnt;
  rmsL = std::sqrt(rmsL / cnt);

  const float enNorm = std::min(1.0f, enL * 6.0f);
  const float bassNorm = std::min(1.0f, bassL * 10.0f);
  const float midNorm = std::min(1.0f, midL * 6.0f);
  const float treNorm = std::min(1.0f, treL * 4.0f);

  // onset: how far instantaneous energy punches above the running envelope
  const float onsetRaw = std::max(0.0f, enNorm - S.envEnergy);
  const float kickRaw = std::max(0.0f, bassNorm - S.envBass);
  S.envEnergy = std::max(enNorm, S.envEnergy * 0.93f);
  S.envBass = std::max(bassNorm, S.envBass * 0.9f);

  const float centroid = 0.35f * bassNorm + 0.55f * midNorm + 0.75f * treNorm;
  S.smRms += (std::min(1.0f, rmsL * 2.2f) - S.smRms) * 0.5f;
  S.smCentroid += (std::min(1.0f, centroid) - S.smCentroid) * 0.3f;
  S.smOnset += (std::min(1.0f, onsetRaw * 5.0f) - S.smOnset) * 0.7f;
  S.smKick += (std::min(1.0f, kickRaw * 7.0f) - S.smKick) * 0.8f;

  react.bass.store(bassNorm);
  react.mid.store(midNorm);
  react.treble.store(treNorm);
  react.energy.store(enNorm);
  react.rms.store(S.smRms);
  react.centroid.store(S.smCentroid);
  react.onset.store(S.smOnset);
  react.kick.store(S.smKick);
  react.peak.store(peakL);

  S.frame += frames;

  // live FFT spectrogram: capture the block's mixed output (runs on whatever
  // thread called the callback - the audio thread, or the main thread's
  // wall-clock fallback; never concurrently, so the rolling window below is
  // lock-free. Only the ring publish is shared with the editor's reader.)
  captureSpectrum(out, frames);
}

// ---------------------------------------------------------------------------
// live FFT spectrogram ring
// ---------------------------------------------------------------------------
/** iterative radix-2 FFT in place (re/im interleaved arrays), n power of two */
static void fftRadix2(float* re, float* im, unsigned n) {
  for (unsigned i = 1, j = 0; i < n; i++) {
    unsigned bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (unsigned len = 2; len <= n; len <<= 1) {
    const float ang = -6.2831853f / (float)len;
    const float wr = std::cos(ang), wi = std::sin(ang);
    for (unsigned i = 0; i < n; i += len) {
      float tr = 1.0f, ti = 0.0f;
      for (unsigned k = 0; k < len / 2; k++) {
        const unsigned a = i + k, b = i + k + len / 2;
        const float xr = re[b] * tr - im[b] * ti;
        const float xi = re[b] * ti + im[b] * tr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
        const float nwr = tr * wr - ti * wi;
        ti = tr * wi + ti * wr;
        tr = nwr;
      }
    }
  }
}

void AudioEngine::captureSpectrum(const float* out, unsigned frames) {
  // mono-mix the block into the rolling window; emit one FFT column every
  // kSpecHop frames once the first window is full
  for (unsigned f = 0; f < frames; f++) {
    specIn_[specInPos_] = (out[f * 2] + out[f * 2 + 1]) * 0.5f;
    specInPos_ = (specInPos_ + 1) & (kSpecN - 1);
    specFilled_++;
    if ((specFilled_ & (kSpecHop - 1)) == 0 && specFilled_ >= kSpecN) {
      emitSpectrumColumn();
    }
  }
}

void AudioEngine::emitSpectrumColumn() {
  // window the last kSpecN mono frames (Hann) and FFT them. Fixed scratch on
  // the audio thread (kSpecN*2 floats) - no allocation in the callback.
  static thread_local float re[kSpecN], im[kSpecN];
  const uint32_t start = (specInPos_ - kSpecN) & (kSpecN - 1);
  for (unsigned i = 0; i < kSpecN; i++) {
    const float x = specIn_[(start + i) & (kSpecN - 1)];
    const float w = 0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)(kSpecN - 1));
    re[i] = x * w;
    im[i] = 0.0f;
  }
  fftRadix2(re, im, kSpecN);

  // log-spaced magnitude bands (40 Hz .. 18 kHz), dB-compressed relative to
  // the column's own peak so quiet and loud sources both fill the heat strip
  float binMag[kSpecBins];
  const float sr = (float)sampleRate_;
  const float fLow = 40.0f, fHigh = 18000.0f;
  const float ratio = std::pow(fHigh / fLow, 1.0f / (float)kSpecBins);
  float peak = 1e-9f;
  for (unsigned b = 0; b < kSpecBins; b++) {
    const int k0 = std::max(1, (int)(fLow * std::pow(ratio, (float)b) * (float)kSpecN / sr));
    const int k1 = std::min((int)(kSpecN / 2),
                            (int)(fLow * std::pow(ratio, (float)(b + 1)) * (float)kSpecN / sr));
    float mag = 0;
    for (int k = k0; k <= k1; k++) mag += std::sqrt(re[k] * re[k] + im[k] * im[k]);
    binMag[b] = mag / (float)(k1 - k0 + 1);
    peak = std::max(peak, binMag[b]);
  }

  // publish: fully write the slot, then expose it with the count increment.
  // An absolute floor guards the silence case - normalizing a near-silent
  // column to its own peak would turn every bin bright (mag/peak ~= 1).
  const uint32_t slot = specCount_.load(std::memory_order_relaxed) % kSpecCap;
  const double center = (double)(specFilled_ - kSpecN / 2) / (double)sampleRate_;
  const bool silent = peak < 1e-4f;
  for (unsigned b = 0; b < kSpecBins; b++) {
    float v = 0.0f;
    if (!silent) {
      v = 20.0f * std::log10(binMag[b] / peak + 1e-6f);
      v = (v + 45.0f) / 45.0f;  // -45 dB floor -> 0..1
    }
    spec_[slot * kSpecBins + b] = v < 0 ? 0.0f : (v > 1 ? 1.0f : v);
  }
  specT_[slot] = (float)center;
  specCount_.fetch_add(1, std::memory_order_release);
}

void audioCallback(void* userdata, float* out, unsigned frames) {
  AudioEngine* eng = (AudioEngine*)userdata;
  const unsigned sr = eng->sampleRate_;

  // --- external track: copy the decoded WAV verbatim, then run the analyser
  // on those samples (no synth in the chain, so the original music plays
  // clean and the react values track the real audio - exactly like the web
  // build).
  if (eng->trackMode && eng->trackFrames_ > 0) {
    for (unsigned f = 0; f < frames; f++) {
      const uint64_t pos = eng->trackPos_.load() + f;
      float l = 0, r = 0;
      if (pos < eng->trackFrames_) {
        const size_t off = (size_t)pos * eng->trackChannels_;
        l = eng->trackData_[off];
        r = eng->trackChannels_ > 1 ? eng->trackData_[off + 1] : l;
      }
      out[f * 2 + 0] = l;
      out[f * 2 + 1] = r;
    }
    // warm the analyser bands over the whole block (so the first block's
    // kick/bass readings aren't cold)
    for (unsigned f = 0; f < frames; f++) {
      const float x = (out[f * 2] + out[f * 2 + 1]) * 0.5f;
      eng->analyser_.bandBass.process(x); eng->analyser_.bandMid.process(x);
      eng->analyser_.bandTre.process(x);
    }
    eng->trackPos_ += frames;
    if (eng->trackFrames_ > 0 && eng->trackPos_.load() >= eng->trackFrames_)
      eng->trackPos_.store(eng->trackPos_.load() % eng->trackFrames_);  // loop past EOF
  } else {
    // no track: silence (the built-in synth is gone). The analyser still
    // runs so the show clock keeps advancing and the spectrum ring keeps
    // publishing quiet columns - the visuals simply have no audio to react to.
    std::memset(out, 0, (size_t)frames * 2 * sizeof(float));
  }

  eng->analyzeAndAdvance(out, frames);
  eng->frameCursor_.store((double)eng->analyser_.frame);
}

// --- real playback device ------------------------------------------------------
// miniaudio data callback (audio thread): feeds the shared audioCallback's
// output straight into the device. pUserData carries the AudioEngine.
static void deviceDataCallback(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
  (void)dev;
  (void)in;
  AudioEngine* eng = static_cast<AudioEngine*>(dev->pUserData);
  audioCallback(eng, static_cast<float*>(out), frames);
}

AudioEngine::~AudioEngine() {
  // a worker may still be decoding (editor closed mid-swap): signal it and
  // wait so it never touches members after they are destroyed
  cancelWorker_ = true;
  if (worker_.joinable()) worker_.join();
  if (device_) {
    ma_device_uninit(device_);
    delete device_;
    device_ = nullptr;
  }
}

bool AudioEngine::init() {
  if (inited_) return true;
  sampleRate_ = 48000;
  specIn_.assign(kSpecN, 0.0f);
  spec_.assign((size_t)kSpecCap * kSpecBins, 0.0f);
  specT_.assign(kSpecCap, 0.0f);
  // A real playback device is opened lazily in start() when the show begins
  // (see startDevice). If no device can be opened (no sound card / driver
  // quirk) the engine falls back to wall-clock rendering in update() so the
  // show clock + visuals always run - just silently.
  inited_ = true;
  return true;
}

AudioEngine::Decoded AudioEngine::decodeFile(const std::string& path,
                                              std::atomic<bool>& cancel) {
  Decoded out;
  // Decode the whole file via miniaudio's bundled dr_wav/dr_mp3 (format
  // auto-detected from the content, so WAV and MP3 both work). The decoder
  // config requests stereo float at our engine sample rate, so mono / 44.1k
  // sources are converted + resampled internally by miniaudio; the show clock
  // (trackPos_ / sampleRate_) then stays in real seconds exactly like the web
  // build's audio.currentTime. The worker checks `cancel` between chunk reads
  // so a torn-down engine exits promptly on large files. The whole decode is
  // serialized by gDecodeMtx (see its comment: concurrent miniaudio decoders
  // wedge each other).
  std::lock_guard<std::mutex> dk(gDecodeMtx);
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, sampleRate_);
  ma_decoder dec;
  // Load the track bytes through the runtime VFS (dev tree or package), so
  // --track works with both a filesystem path and a virtual path. The bytes
  // must outlive the decoder; decodeFile fully drains it before returning.
  std::vector<uint8_t> bytes = runtimeFS().read(path);
  if (bytes.empty()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
      std::ifstream f(path, std::ios::binary);
      if (f) bytes.assign(std::istreambuf_iterator<char>(f),
                          std::istreambuf_iterator<char>());
    }
  }
  const ma_result openRes = bytes.empty()
                                ? MA_DOES_NOT_EXIST
                                : ma_decoder_init_memory(bytes.data(), bytes.size(),
                                                         &cfg, &dec);
  if (openRes != MA_SUCCESS) {
    std::fprintf(stderr,
                 "[AUDIO] track load failed: cannot decode '%s' (WAV/MP3);"
                 " keeping the previous source (silent if none).\n",
                 path.c_str());
    return out;
  }

  std::vector<float> data;
  std::vector<float> tmp(65536 * 2);  // 64k stereo frames scratch
  for (;;) {
    if (cancel.load()) break;  // teardown / superseded mid-decode: drop it
    ma_uint64 n = 0;
    const ma_result r = ma_decoder_read_pcm_frames(&dec, tmp.data(), 65536, &n);
    if (r != MA_SUCCESS || n == 0) break;
    data.insert(data.end(), tmp.begin(), tmp.begin() + (size_t)n * 2);
  }
  ma_decoder_uninit(&dec);

  if (cancel.load()) return out;  // canceled: caller publishes nothing useful
  if (data.empty()) {
    std::fprintf(stderr,
                 "[AUDIO] track load failed: decoded 0 frames from '%s';"
                 " keeping the previous source (silent if none).\n",
                 path.c_str());
    return out;
  }
  out.ok = true;
  out.data = std::move(data);
  out.frames = out.data.size() / 2;
  return out;
}

void AudioEngine::loadTrack(const std::string& path) {
  trackPath_ = path;
  trackMode = false;

  // startup path (--track= / auto-find): decode on the caller's thread via
  // the same serialized decodeFile the async worker uses (see gDecodeMtx).
  Decoded d = decodeFile(path, cancelWorker_);
  if (!d.ok) return;  // decode already printed the reason; keep silence

  trackData_ = std::move(d.data);
  trackFrames_ = d.frames;
  trackChannels_ = 2;
  trackPos_ = 0;
  trackMode = true;
  trackDuration = (float)((double)trackFrames_ / (double)sampleRate_);
  std::printf("[AUDIO] track loaded: %s (%.2fs, %u Hz stereo f32)\n",
              path.c_str(), trackDuration, sampleRate_);
}

bool AudioEngine::swapTrack(const std::string& path, float seekSec) {
  // synchronous wrapper: start an async decode, wait for it, commit. The
  // caller blocks for the decode duration (startup/restore paths, smokes);
  // the editor's UI path uses beginAsyncSwap + applyAsyncSwap instead.
  beginAsyncSwap(path, seekSec);
  while (asyncStatus() == AsyncState::Decoding) std::this_thread::yield();
  return applyAsyncSwap();
}

void AudioEngine::beginAsyncSwap(const std::string& path, float seekSec) {
  // supersede any in-flight swap: a torn-down engine must not have its worker
  // publish into a half-read pending slot, so cancel + join before reuse.
  cancelWorker_ = true;
  if (worker_.joinable()) worker_.join();
  cancelWorker_ = false;

  {
    std::lock_guard<std::mutex> lk(publishMtx_);
    if (path.empty()) {
      // stop audio (silence) - no decode needed, commit immediately
      pendingPath_.clear();
      pendingData_.clear();
      pendingFrames_ = 0;
      pendingSeekSec_ = 0;
      asyncState_ = AsyncState::Ready;
      return;
    }
    pendingPath_ = path;
    pendingData_.clear();
    pendingFrames_ = 0;
    pendingSeekSec_ = seekSec;
    asyncState_ = AsyncState::Decoding;
  }

  // The old track keeps playing while this worker decodes. The worker only
  // touches the pending slot + the local copy of path (see asyncPath), both
  // under publishMtx_; the live track fields are untouched until the main
  // thread commits via applyAsyncSwap (device stopped, buffer swapped).
  const uint64_t gen = ++asyncGen_;
  worker_ = std::thread([this, path, gen] {
    Decoded d = decodeFile(path, cancelWorker_);
    std::lock_guard<std::mutex> lk(publishMtx_);
    if (gen != asyncGen_) return;  // superseded: a newer swap owns the slot
    if (d.ok) {
      pendingData_ = std::move(d.data);
      pendingFrames_ = d.frames;
      asyncState_ = AsyncState::Ready;
    } else {
      pendingData_.clear();
      pendingFrames_ = 0;
      asyncState_ = AsyncState::Failed;
    }
  });
}

AudioEngine::AsyncState AudioEngine::asyncStatus() const {
  std::lock_guard<std::mutex> lk(publishMtx_);
  return asyncState_;
}

std::string AudioEngine::asyncPath() const {
  std::lock_guard<std::mutex> lk(publishMtx_);
  return pendingPath_;
}

void AudioEngine::applyReady(const std::string& path, float seekSec) {
  // The audio callback reads trackMode / trackData_ / trackPos_ directly on
  // the audio thread, so a runtime swap must happen while the device is
  // stopped (ma_device_stop drains the callback): move-assigning trackData_
  // under a callback mid-copy would free the buffer it is reading from.
  if (device_) ma_device_stop(device_);

  const bool toSilent = path.empty();
  if (toSilent) {
    // stop audio: the show runs silent. A silent run keeps no device open -
    // the wall-clock fallback in update() keeps the show clock + analyser
    // advancing, so the timeline never freezes.
    trackMode = false;
    trackData_.clear();
    trackData_.shrink_to_fit();
    trackFrames_ = 0;
    trackChannels_ = 2;
    trackPos_ = 0;
    trackPath_.clear();
    trackDuration = 0;
    if (device_) {
      ma_device_uninit(device_);
      delete device_;
      device_ = nullptr;
    }
    deviceLive_ = false;
  } else {
    trackData_ = std::move(pendingData_);
    trackFrames_ = pendingFrames_;
    trackChannels_ = 2;
    trackPath_ = path;
    trackMode = true;
    trackDuration = (float)((double)trackFrames_ / (double)sampleRate_);
    std::printf("[AUDIO] track loaded: %s (%.2fs, %u Hz stereo f32)\n",
                path.c_str(), trackDuration, sampleRate_);
    if (device_) {
      // resume the paused device with the new buffer
      ma_device_start(device_);
      deviceLive_ = true;
    } else if (startDevice()) {
      // the engine booted silent (no track -> no device): open one now so
      // the committed track is actually heard
      deviceLive_ = true;
    }
  }
  if (seekSec > 0) {
    trackPos_ =
        (uint64_t)std::min((double)seekSec * sampleRate_, (double)trackFrames_);
  }
}

bool AudioEngine::applyAsyncSwap() {
  // commit a Ready decode on the main thread: move the pending buffer in with
  // a single device stop/start. The previous source keeps playing right up to
  // this point (the decode already finished), so the audible gap is just the
  // buffer swap - milliseconds, not the whole file.
  std::string path;
  float seekSec = 0;
  {
    std::lock_guard<std::mutex> lk(publishMtx_);
    if (asyncState_ != AsyncState::Ready && asyncState_ != AsyncState::Failed) {
      return false;  // still decoding / nothing pending
    }
    path = pendingPath_;
    seekSec = pendingSeekSec_;
    if (asyncState_ == AsyncState::Failed) {
      // decode failed: the previous source is kept untouched
      asyncState_ = AsyncState::Idle;
      pendingPath_.clear();
      pendingData_.clear();
      pendingFrames_ = 0;
      return false;
    }
  }
  applyReady(path, seekSec);
  {
    std::lock_guard<std::mutex> lk(publishMtx_);
    asyncState_ = AsyncState::Idle;
    pendingPath_.clear();
    pendingData_.clear();
    pendingFrames_ = 0;
  }
  return true;
}

void AudioEngine::seekTrack(float sec) {
  if (!trackMode) return;
  const double s = std::max(0.0, (double)sec);
  const uint64_t target = (uint64_t)std::min(s * sampleRate_, (double)trackFrames_);
  if (target == trackPos_.load()) return;  // already there
  // plain atomic store, NO device stop/start: the callback re-reads trackPos_
  // every sample, so the playhead just jumps (exactly what scrubbing wants -
  // no audible click, the show clock keeps running). Device stop/start is
  // reserved for swapTrack, where the decoded buffer is actually replaced.
  trackPos_.store(target);
}

bool AudioEngine::startDevice() {
  if (device_) return true;
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = sampleRate_;
  cfg.dataCallback = deviceDataCallback;
  cfg.pUserData = this;
  device_ = new ma_device();
  if (ma_device_init(NULL, &cfg, device_) != MA_SUCCESS) {
    std::fprintf(stderr, "[AUDIO] no playback device - running the show clock on wall time (silent)\n");
    delete device_;
    device_ = nullptr;
    return false;
  }
  if (ma_device_start(device_) != MA_SUCCESS) {
    std::fprintf(stderr, "[AUDIO] playback device failed to start - running the show clock on wall time (silent)\n");
    ma_device_uninit(device_);
    delete device_;
    device_ = nullptr;
    return false;
  }
  // a plain [AUDIO] line (not the boot banner): this is also reachable at
  // runtime, when the first track loads after a silent boot
  std::printf("[AUDIO] playback device live\n");
  return true;
}

void AudioEngine::prepareAnalyser() {
  // the analyser bands (delay/reverb belonged to the removed synth)
  analyser_.bandBass.lowpass(750.0f, 0.7f, (float)sampleRate_);
  analyser_.bandMid.bandpass(1800.0f, 0.7f, (float)sampleRate_);
  analyser_.bandTre.highpass(3200.0f, 0.7f, (float)sampleRate_);
}

void AudioEngine::start() {
  if (started) return;
  started = true;
  lastWall_ = 0;  // prime update()'s wall clock on the first frame
  prepareAnalyser();
  if (!trackMode) {
    // no track file: the show runs silent, and no device is opened. The show
    // clock then depends on update() being polled every frame (its wall-clock
    // fallback renders silence blocks, keeping now() advancing) - the plain
    // demo loop and the editor both do this; any headless mode that skips
    // update() would freeze the clock at t=0.
    std::printf("[AUDIO] no track file - running silent (drop a WAV/MP3 to add music)\n");
    return;
  }
  // open a real playback device so the track is actually heard; when that
  // fails the show clock + visuals keep running on the wall clock via
  // update() instead (silent fallback).
  if (startDevice()) deviceLive_ = true;
}

float AudioEngine::now() const {
  if (!started) return 0;
  return (float)(frameCursor_.load() / sampleRate_);
}

bool AudioEngine::selfTest() {
  // headless playback check for --smoke-audio: feed the track branch of
  // audioCallback one 2048-frame block and assert it copied non-silent
  // samples, advanced the show clock by exactly that block, and produced
  // analyser energy (the copy/analysis path is otherwise only exercised
  // inside the render loop, which the smoke run exits before).
  if (!trackMode || trackFrames_ == 0) {
    std::fprintf(stderr, "[AUDIO-SMOKE] no track loaded (no audio.wav/audio.mp3 found in assets, exe dir, or cwd)\n");
    return false;
  }
  // configure the analyser bands WITHOUT opening a real device (this is a
  // headless check; start() would start streaming to the speakers)
  started = true;
  prepareAnalyser();
  const uint64_t pos0 = trackPos_.load();
  const double clock0 = now();
  thread_local static float buf[2048 * 2];
  std::memset(buf, 0, sizeof(buf));
  audioCallback(this, buf, 2048);
  bool nonSilent = false;
  for (size_t i = 0; i < 2048 * 2 && !nonSilent; i++) nonSilent = buf[i] != 0.0f;
  // a track shorter than one block (~43ms) clamps at EOF, so compare against
  // the frames actually copied, not a hard 2048
  const uint64_t copied = trackPos_.load() - pos0;
  const bool advanced = copied > 0 && trackPos_.load() <= trackFrames_ && now() > clock0;
  const float energy = react.energy.load();
  const bool analysed = energy > 0.0f;
  // the FFT spectrogram ring must have accumulated columns with real content
  const uint32_t cols = spectrumCount();
  bool spectrumOk = cols > 0;
  if (spectrumOk) {
    const float* last = spectrumColumn(cols - 1);
    float mx = 0;
    for (unsigned b = 0; b < kSpecBins; b++) mx = std::max(mx, last[b]);
    spectrumOk = mx > 0.01f;
  }
  const bool ok = nonSilent && advanced && analysed && spectrumOk;
  std::printf("[AUDIO-SMOKE] frames=%llu/%llu clock=%.3fs energy=%.3f spec=%d/%u"
              " nonSilent=%d -> %s\n",
              (unsigned long long)copied, (unsigned long long)trackFrames_,
              now(), energy, spectrumOk ? 1 : 0, cols, nonSilent ? 1 : 0,
              ok ? "PASS" : "FAIL");
  return ok;
}

void AudioEngine::update() {
  // A live playback device drives the show clock + analyser from its data
  // callback on the audio thread, so update() is a no-op in that mode. It is
  // only the fallback when no device could be opened (or the show runs
  // silent with no track): render the callback in elapsed-sized blocks on
  // the wall clock so the show clock + visuals still run (silently).
  if (deviceLive_) {
    // stall guard: if the device started "successfully" but its data callback
    // never fires (headless VM / driver quirk), frameCursor_ stops advancing
    // and the show clock would freeze. After ~3s of no advance, drop the
    // device flag so the wall-clock renderer below takes over.
    const double cur = frameCursor_.load();
    if (cur == lastCursor_) {
      if (++stallFrames_ > 180) {
        deviceLive_ = false;
        lastWall_ = 0;  // re-prime the wall clock
        if (device_) ma_device_stop(device_);  // stop the silent stream
        std::fprintf(stderr, "[AUDIO] device stalled - falling back to the wall clock (silent)\n");
      }
    } else {
      stallFrames_ = 0;
      lastCursor_ = cur;
    }
    return;
  }
  if (!started || !inited_) return;
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (lastWall_ <= 0) { lastWall_ = wall; return; }
  const double elapsed = wall - lastWall_;
  lastWall_ = wall;
  if (elapsed <= 0 || elapsed > 0.25) return;  // clamp paused/burst gaps
  double framesD = elapsed * sampleRate_;
  thread_local static float buf[2048 * 2];
  while (framesD > 0) {
    // (unsigned) truncates, so a sub-block remainder like 0.8 would become 0
    // and the loop would spin forever on `framesD -= 0` - the classic
    // freeze-after-click on 59.94 Hz displays (48000*1/60 = 800 exactly,
    // but any real-world refresh leaves a fraction). Drop remainders < 1
    // frame instead: they are under one audio sample (~20us) of error.
    const double block = std::min<double>(framesD, 2048);
    if (block < 1.0) break;
    const unsigned n = (unsigned)block;
    audioCallback(this, buf, n);
    framesD -= n;
  }
}

}  // namespace ns
