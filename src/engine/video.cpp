#include "engine/video.hpp"

#include "framework/core/ffmpegpipe.hpp"
#include "framework/core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace ns {
namespace {

std::string shellQuote(const std::string& value) {
#ifdef _WIN32
  std::string out = "\"";
  for (char c : value) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  out += "\"";
  return out;
#else
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
#endif
}

}  // namespace

VideoPlayer::~VideoPlayer() { close(); }

bool VideoPlayer::open(const std::string& path, int width, int height, float fps, bool loop) {
  close();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    Log::error("VIDEO", "file not found: " + path);
    failed_.store(true, std::memory_order_release);
    return false;
  }
  path_ = path;
  width_ = std::max(2, width);
  height_ = std::max(2, height);
  fps_ = std::max(1.0f, fps);
  loop_ = loop;
  stop_.store(false, std::memory_order_release);
  ready_.store(false, std::memory_order_release);
  failed_.store(false, std::memory_order_release);
  worker_ = std::thread(&VideoPlayer::workerMain, this);
  return true;
}

void VideoPlayer::close() {
  stop_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  texture_.destroy();
  {
    std::lock_guard<std::mutex> lock(frameMtx_);
    pendingFrame_.clear();
    pending_ = false;
  }
  ready_.store(false, std::memory_order_release);
}

std::string VideoPlayer::command() const {
  // -re makes ffmpeg emit frames in realtime instead of decoding the whole
  // clip immediately. The newest-frame mailbox below intentionally drops
  // frames when rendering is slower than the source.
  return "ffmpeg -hide_banner -loglevel error -re -i " + shellQuote(path_) +
         " -f rawvideo -pix_fmt rgba -vf scale=" + std::to_string(width_) +
         ":" + std::to_string(height_) + ",fps=" + std::to_string(fps_) +
         " -vsync 0 -an -";
}

void VideoPlayer::workerMain() {
  const size_t frameBytes = (size_t)width_ * (size_t)height_ * 4u;
  bool decodedAny = false;
  while (!stop_.load(std::memory_order_acquire)) {
    FILE* pipe = openPipe(command(), "rb");
    if (!pipe) {
      failed_.store(true, std::memory_order_release);
      Log::error("VIDEO", "could not start ffmpeg for " + path_);
      return;
    }
    std::vector<uint8_t> frame(frameBytes);
    while (!stop_.load(std::memory_order_acquire)) {
      const size_t got = std::fread(frame.data(), 1, frameBytes, pipe);
      if (got != frameBytes) break;
      {
        std::lock_guard<std::mutex> lock(frameMtx_);
        pendingFrame_ = frame;
        pending_ = true;
      }
      decodedAny = true;
      ready_.store(true, std::memory_order_release);
    }
    closePipe(pipe);
    if (!loop_ || stop_.load(std::memory_order_acquire)) break;
  }
  if (!decodedAny) failed_.store(true, std::memory_order_release);
}

void VideoPlayer::update() {
  std::vector<uint8_t> frame;
  {
    std::lock_guard<std::mutex> lock(frameMtx_);
    if (!pending_) return;
    frame.swap(pendingFrame_);
    pending_ = false;
  }
  if (frame.empty()) return;
  // ffmpeg writes rawvideo rows top-to-bottom, while OpenGL texture uploads
  // are addressed from the bottom through the engine's conventional v=0 UV.
  // Flip the mailbox frame once before upload so video has the same
  // orientation as the rest of the textured-quad pipeline.
  const size_t rowBytes = (size_t)width_ * 4u;
  for (int y = 0; y < height_ / 2; ++y) {
    auto* a = frame.data() + (size_t)y * rowBytes;
    auto* b = frame.data() + (size_t)(height_ - 1 - y) * rowBytes;
    for (size_t x = 0; x < rowBytes; ++x) std::swap(a[x], b[x]);
  }
  if (!texture_.tex || texture_.w != width_ || texture_.h != height_)
    texture_ = Texture::fromRGBA(width_, height_, frame.data(),
                                 {::gl::LINEAR, ::gl::LINEAR, ::gl::CLAMP_TO_EDGE, false});
  else
    texture_.updateRGBA(width_, height_, frame.data());
}

}  // namespace ns
