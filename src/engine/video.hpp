// ---------------------------------------------------------------------------
// VideoPlayer - lightweight realtime video playback for scene nodes.
//
// Decoding is intentionally kept out of the renderer: ffmpeg produces a
// scaled RGBA frame stream on a worker thread, while the main thread uploads
// the newest complete frame into one reusable GL texture. This uses the same
// ffmpeg executable already required by MP4 export and keeps the runtime
// player free of a large codec library.
// ---------------------------------------------------------------------------
#pragma once

#include "engine/texture.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ns {

class VideoPlayer {
public:
  VideoPlayer() = default;
  ~VideoPlayer();
  VideoPlayer(const VideoPlayer&) = delete;
  VideoPlayer& operator=(const VideoPlayer&) = delete;

  /** Start decoding PATH. Width/height are the upload size; ffmpeg scales the
   * source to this size, avoiding per-frame texture reallocations. */
  bool open(const std::string& path, int width = 1280, int height = 720,
            float fps = 30.0f, bool loop = true);
  void close();

  /** Move the newest decoded frame to the GL texture. Main-thread only. */
  void update();
  Texture* texture() { return texture_.tex ? &texture_ : nullptr; }
  const Texture* texture() const { return texture_.tex ? &texture_ : nullptr; }
  bool ready() const { return ready_.load(std::memory_order_acquire); }
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  const std::string& path() const { return path_; }
  int width() const { return width_; }
  int height() const { return height_; }
  float fps() const { return fps_; }

private:
  void workerMain();
  std::string command() const;

  std::string path_;
  int width_ = 1280;
  int height_ = 720;
  float fps_ = 30.0f;
  bool loop_ = true;
  Texture texture_;

  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> failed_{false};
  mutable std::mutex frameMtx_;
  std::vector<uint8_t> pendingFrame_;
  bool pending_ = false;
};

}  // namespace ns
