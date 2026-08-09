// ---------------------------------------------------------------------------
// Mp4Export - the demo editor's MP4 capture pipeline (File > Export MP4...).
//
// Runs the same ffmpeg pipe the CLI --export-mp4 path uses. The editor's
// frame loop hands each presented frame (glReadPixels of the default
// framebuffer) to a background writer thread, so a slow ffmpeg can never
// stall the editor UI: the writer drains a bounded buffer pool, and frames
// pushed while the pool is exhausted are dropped (and counted) rather than
// queued without limit.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/ffmpegpipe.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ns {

class Mp4Export {
public:
  enum class State { Idle, Running, Done, Failed, Cancelled };

  Mp4Export() = default;
  ~Mp4Export();  // cancels a running export
  Mp4Export(const Mp4Export&) = delete;
  Mp4Export& operator=(const Mp4Export&) = delete;

  /** start exporting to outPath at w x h / fps; audioPath (a real file) is
   *  muxed when non-empty. Returns false + error() when ffmpeg is missing
   *  or the pipe cannot be opened. */
  bool start(const std::string& outPath, int w, int h, float fps,
             const std::string& audioPath);

  /** capture the current default-framebuffer frame (call on the GL thread,
   *  right after the show render). Uses the dimensions from start(), so a
   *  mid-export window resize cannot feed the pipe a different size.
   *  Drops - and counts - when the writer's bounded buffer pool is
   *  exhausted. */
  void pushFrame();

  /** stop capturing, flush the remaining frames to ffmpeg, close the pipe. */
  void finish();
  /** abort: drain what is queued, close the pipe (a partial MP4 stays valid). */
  void cancel();

  State state() const { return state_; }
  bool running() const { return state_ == State::Running; }

  /** frames actually written to ffmpeg so far */
  size_t framesWritten() const;
  /** frames dropped because the writer fell behind */
  size_t framesDropped() const;
  int exitCode() const { return exitCode_; }
  const std::string& error() const { return error_; }
  const std::string& path() const { return path_; }

private:
  void writerLoop();
  void pushBytes(const unsigned char* rgb, size_t n);

  State state_ = State::Idle;
  std::string path_, error_;
  FILE* pipe_ = nullptr;
  std::thread thread_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::vector<unsigned char>> queue_;  // frames waiting to be written
  std::deque<std::vector<unsigned char>> free_;   // pooled buffers, ready to fill
  static constexpr size_t kPoolMax = 4;
  bool stop_ = false;
  bool writeFailed_ = false;
  size_t written_ = 0;
  size_t dropped_ = 0;
  int exitCode_ = 0;
  int w_ = 0, h_ = 0;  // capture dimensions (fixed at start)
  std::vector<unsigned char> scratch_;  // GL-thread readback buffer (reused)
};

}  // namespace ns
