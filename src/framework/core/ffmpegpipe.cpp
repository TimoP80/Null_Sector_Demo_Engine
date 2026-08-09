#include "framework/core/ffmpegpipe.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ns {

std::string buildFfmpegCaptureCmd(const std::string& outPath, int w, int h,
                                  float fps, const std::string& audioPath) {
  char fpsBuf[32];
  std::snprintf(fpsBuf, sizeof(fpsBuf), "%g", fps);
  std::string cmd =
      "ffmpeg -y -hide_banner -loglevel warning -f rawvideo -pix_fmt rgb24 -s ";
  cmd += std::to_string(w) + "x" + std::to_string(h);
  cmd += " -r " + std::string(fpsBuf) + " -i -";
  if (!audioPath.empty()) cmd += " -i \"" + audioPath + "\"";
  cmd += " -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart";
  if (!audioPath.empty()) cmd += " -c:a aac -b:a 192k -shortest";
  cmd += " \"" + outPath + "\"";
  return cmd;
}

void flipRowsInPlace(unsigned char* rgb, int w, int h) {
  if (w <= 0 || h <= 1) return;
  const size_t row = (size_t)w * 3;
  std::vector<unsigned char> tmp(row);
  for (int y = 0; y < h / 2; y++) {
    unsigned char* a = rgb + (size_t)y * row;
    unsigned char* b = rgb + (size_t)(h - 1 - y) * row;
    std::memcpy(tmp.data(), a, row);
    std::memcpy(a, b, row);
    std::memcpy(b, tmp.data(), row);
  }
}

FILE* openPipe(const std::string& cmd, const char* mode) {
#ifdef _WIN32
  return _popen(cmd.c_str(), mode);
#else
  return popen(cmd.c_str(), mode);
#endif
}

int closePipe(FILE* p) {
#ifdef _WIN32
  return _pclose(p);
#else
  return pclose(p);
#endif
}

}  // namespace ns
