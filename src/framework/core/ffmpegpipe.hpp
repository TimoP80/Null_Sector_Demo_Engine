// ---------------------------------------------------------------------------
// Shared ffmpeg pipe helpers for MP4 export - used by the CLI --export-mp4
// path (main.cpp) and the demo editor's File > Export MP4... action, so both
// capture pipelines build the exact same ffmpeg command. GL-free.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <string>

namespace ns {

/** build the ffmpeg command that consumes raw RGB24 frames on stdin and
 *  writes an H.264 MP4: rawvideo rgb24 -s WxH -r fps -i -, plus the muxed
 *  audio track (aac 192k, -shortest) when audioPath is non-empty. */
std::string buildFfmpegCaptureCmd(const std::string& outPath, int w, int h,
                                  float fps, const std::string& audioPath);

/** vertically flip an RGB row-major frame in place. glReadPixels returns
 *  rows bottom-up (row 0 = the image bottom); ffmpeg's rawvideo input
 *  expects top-down, so every captured frame must be flipped. */
void flipRowsInPlace(unsigned char* rgb, int w, int h);

/** portable popen/pclose (feeds ffmpeg on stdin). */
FILE* openPipe(const std::string& cmd, const char* mode);
int closePipe(FILE* p);

}  // namespace ns
