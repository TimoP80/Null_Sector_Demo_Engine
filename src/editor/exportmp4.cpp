#include "editor/exportmp4.hpp"

#include "engine/gl.hpp"

#include <cstdio>
#include <cstring>

namespace ns {

Mp4Export::~Mp4Export() {
  if (running()) cancel();
}

bool Mp4Export::start(const std::string& outPath, int w, int h, float fps,
                      const std::string& audioPath) {
  if (running()) return false;
  // availability probe: fail early with a clear message, not mid-export
  FILE* probe = openPipe("ffmpeg -hide_banner -version", "r");
  if (!probe) {
    error_ = "ffmpeg not found on PATH - install it (or add its folder to PATH)";
    state_ = State::Failed;
    return false;
  }
  char probeBuf[128];
  (void)std::fread(probeBuf, 1, sizeof(probeBuf), probe);
  closePipe(probe);

  const std::string cmd = buildFfmpegCaptureCmd(outPath, w, h, fps, audioPath);
  FILE* p = openPipe(cmd, "wb");
  if (!p) {
    error_ = "failed to start ffmpeg for " + outPath;
    state_ = State::Failed;
    return false;
  }
  // pre-fill the buffer pool so the very first pushFrame has a buffer
  // (the writer only returns buffers after it has written one)
  queue_.clear();
  free_.clear();
  for (size_t i = 0; i < kPoolMax; i++)
    free_.push_back(std::vector<unsigned char>((size_t)w * (size_t)h * 3));
  path_ = outPath;
  pipe_ = p;
  w_ = w;
  h_ = h;
  stop_ = false;
  writeFailed_ = false;
  written_ = 0;
  dropped_ = 0;
  exitCode_ = 0;
  error_.clear();
  state_ = State::Running;
  thread_ = std::thread([this] { writerLoop(); });
  return true;
}

void Mp4Export::writerLoop() {
  std::vector<unsigned char> buf;
  while (true) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
    if (queue_.empty() && stop_) break;
    if (!queue_.empty()) {
      buf = std::move(queue_.front());
      queue_.pop_front();
      lk.unlock();
      const bool ok = std::fwrite(buf.data(), 1, buf.size(), pipe_) == buf.size();
      lk.lock();
      free_.push_back(std::move(buf));
      if (!ok) {
        // ffmpeg died (bad path, disk full, ...): stop accepting frames
        writeFailed_ = true;
        stop_ = true;
        cv_.notify_all();
        break;
      }
      written_++;
    }
  }
}

void Mp4Export::pushFrame() {
  if (!running()) return;
  const size_t n = (size_t)w_ * (size_t)h_ * 3;
  scratch_.resize(n);
  ::glReadPixels(0, 0, w_, h_, ::gl::RGB, ::gl::UNSIGNED_BYTE, scratch_.data());
  flipRowsInPlace(scratch_.data(), w_, h_);  // GL rows are bottom-up; rawvideo is top-down
  pushBytes(scratch_.data(), n);
}

void Mp4Export::pushBytes(const unsigned char* rgb, size_t n) {
  std::unique_lock<std::mutex> lk(m_);
  if (!running()) return;
  if (free_.empty()) {
    // the writer is behind: drop this frame rather than grow without bound
    dropped_++;
    return;
  }
  std::vector<unsigned char> buf = std::move(free_.front());
  free_.pop_front();
  lk.unlock();
  buf.resize(n);  // first use allocates; afterwards it is already sized
  std::memcpy(buf.data(), rgb, n);
  lk.lock();
  queue_.push_back(std::move(buf));
  cv_.notify_one();
}

void Mp4Export::finish() {
  if (!running()) return;
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
  const int rc = closePipe(pipe_);
  pipe_ = nullptr;
  exitCode_ = rc;
  state_ = (writeFailed_ || rc != 0) ? State::Failed : State::Done;
  if (state_ == State::Failed && error_.empty())
    error_ = "ffmpeg exited with code " + std::to_string(rc);
}

void Mp4Export::cancel() {
  if (!running()) return;
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_ = true;
    cv_.notify_all();
  }
  if (thread_.joinable()) thread_.join();
  const int rc = closePipe(pipe_);
  pipe_ = nullptr;
  exitCode_ = rc;
  state_ = State::Cancelled;
}

size_t Mp4Export::framesWritten() const {
  std::lock_guard<std::mutex> lk(m_);
  return written_;
}

size_t Mp4Export::framesDropped() const {
  std::lock_guard<std::mutex> lk(m_);
  return dropped_;
}

}  // namespace ns
