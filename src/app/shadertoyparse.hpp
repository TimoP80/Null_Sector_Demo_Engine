// ---------------------------------------------------------------------------
// shadertoyparse.hpp - Shadertoy pass splitting (GL-free, header-only).
//
// A Shadertoy .glsl file can hold several passes separated by marker lines:
//
//     // pass: common      - shared helpers, prepended to every other pass
//     // pass: buffer_a    - renders into the RGBA16F buffer A target
//     // pass: image       - the final compose pass
//
// splitShadertoyPasses() turns the file into {name, src} passes. Marker
// detection is strict so prose and nested comments can never split a file:
//
//   * a marker is a line whose FIRST comment is `pass:` + a name token, and
//     everything before that comment must be whitespace. This rejects
//         (no `// pass:` markers = image pass)     // prose mention
//         //   // pass: common  - description      // nested mention
//   * the pass name is the first whitespace-delimited token after "pass:",
//     so a trailing comment on the marker line is ignored
//   * a file with no markers is a single image pass (the whole file)
//   * content before the first marker is dropped; content between markers
//     belongs to the preceding marker's pass (marker lines are separators)
//   * empty passes are dropped (they cannot compile anyway)
//
// The splitter never throws (no raw substr arithmetic on found positions -
// the old parser computed a marker-line end as
// `markerPos + full.find('\n', markerPos)`, adding an absolute offset to
// itself, which silently truncated single-pass files mid-comment and threw
// std::out_of_range ("invalid string position") on multi-pass files).
// ---------------------------------------------------------------------------
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

namespace ns {

struct ShadertoyPass {
  std::string name;  // common | buffer_a..d | image (or any author name)
  std::string src;   // GLSL for this pass; the marker line is excluded
};

/** per-file option: the buffer targets of a heavy multi-pass file can render
 *  at a fraction of the output resolution. The line uses the same strict
 *  first-comment rule as pass markers (only whitespace before the comment,
 *  token-exact), so prose can never trigger it:
 *
 *     // option: renderScale 0.5    (buffer passes render at half res)
 *
 *  Returns the value clamped to (0, 1] (buffer size is floor(res * scale)),
 *  or 1.0 (full res) when the file has no such option. Never throws; a
 *  malformed value falls back to 1.0. */
inline float extractShadertoyRenderScale(const std::string& full) {
  size_t offset = 0;
  if (full.size() >= 3 && (unsigned char)full[0] == 0xEF && (unsigned char)full[1] == 0xBB &&
      (unsigned char)full[2] == 0xBF)
    offset = 3;

  size_t lineStart = offset;
  while (lineStart <= full.size()) {
    const size_t nl = full.find('\n', lineStart);
    const size_t lineEnd = nl == std::string::npos ? full.size() : nl;
    size_t lineEndNoCR = lineEnd;
    if (lineEndNoCR > lineStart && full[lineEndNoCR - 1] == '\r') lineEndNoCR--;

    const size_t c = full.find("//", lineStart);
    if (c != std::string::npos && c < lineEndNoCR) {
      bool wsOnly = true;
      for (size_t k = lineStart; k < c; k++) {
        if (full[k] != ' ' && full[k] != '\t') { wsOnly = false; break; }
      }
      if (wsOnly) {
        size_t p = c + 2;
        while (p < lineEndNoCR && (full[p] == ' ' || full[p] == '\t')) p++;
        if (p + 7 <= lineEndNoCR && full.compare(p, 7, "option:") == 0) {
          size_t q = p + 7;
          while (q < lineEndNoCR && (full[q] == ' ' || full[q] == '\t')) q++;
          if (q + 11 <= lineEndNoCR && full.compare(q, 11, "renderScale") == 0) {
            size_t v = q + 11;
            while (v < lineEndNoCR && (full[v] == ' ' || full[v] == '\t')) v++;
            const char* begin = full.c_str() + v;
            char* end = nullptr;
            const float val = std::strtof(begin, &end);
            // the WHOLE token must be the number: strtof stops at the first
            // non-numeric char, so `0.5foo` would otherwise parse as 0.5.
            // Accept only whitespace / CR / LF / EOL after it.
            if (end != begin && val > 0.0f &&
                (*end == '\0' || *end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
              const float clamped = val > 1.0f ? 1.0f : val;
              return clamped;
            }
          }
        }
      }
    }
    if (nl == std::string::npos) break;
    lineStart = nl + 1;
  }
  return 1.0f;
}

inline std::vector<ShadertoyPass> splitShadertoyPasses(const std::string& full) {
  // strip a UTF-8 BOM if present: the strict first-comment rule requires the
  // text before a marker's `//` to be whitespace-only, and a BOM at byte 0
  // would otherwise silently hide a first-line marker (the old substring
  // scan matched anywhere and was BOM-immune)
  size_t offset = 0;
  if (full.size() >= 3 && (unsigned char)full[0] == 0xEF && (unsigned char)full[1] == 0xBB &&
      (unsigned char)full[2] == 0xBF)
    offset = 3;

  struct Marker {
    std::string name;
    size_t lineStart;  // start of the marker line
    size_t lineEnd;    // position of the marker line's '\n' (or full.size())
  };
  std::vector<Marker> markers;

  size_t lineStart = offset;
  while (lineStart <= full.size()) {
    const size_t nl = full.find('\n', lineStart);
    const size_t lineEnd = nl == std::string::npos ? full.size() : nl;
    // strip a trailing '\r' (CRLF files) before scanning the line
    size_t lineEndNoCR = lineEnd;
    if (lineEndNoCR > lineStart && full[lineEndNoCR - 1] == '\r') lineEndNoCR--;

    // the first comment on the line: everything before it must be whitespace
    const size_t c = full.find("//", lineStart);
    if (c != std::string::npos && c < lineEndNoCR) {
      bool wsOnly = true;
      for (size_t k = lineStart; k < c; k++) {
        if (full[k] != ' ' && full[k] != '\t') { wsOnly = false; break; }
      }
      if (wsOnly) {
        size_t p = c + 2;
        while (p < lineEndNoCR && (full[p] == ' ' || full[p] == '\t')) p++;
        if (p + 5 <= lineEndNoCR && full.compare(p, 5, "pass:") == 0) {
          size_t q = p + 5;
          while (q < lineEndNoCR && (full[q] == ' ' || full[q] == '\t')) q++;
          size_t r = q;
          while (r < lineEndNoCR && full[r] != ' ' && full[r] != '\t') r++;
          if (r > q) markers.push_back({full.substr(q, r - q), lineStart, lineEnd});
        }
      }
    }
    if (nl == std::string::npos) break;
    lineStart = nl + 1;
  }

  std::vector<ShadertoyPass> out;
  if (markers.empty()) {
    // single-pass file: the whole content is the image pass
    out.push_back({"image", full});
    return out;
  }
  for (size_t i = 0; i < markers.size(); i++) {
    // the pass source runs from just past the marker line up to the start of
    // the next marker line (or EOF); both bounds are clamped, so a marker at
    // EOF without a trailing newline cannot overflow
    const size_t srcStart = markers[i].lineEnd + 1 < full.size() ? markers[i].lineEnd + 1 : full.size();
    const size_t srcEnd = i + 1 < markers.size() ? markers[i + 1].lineStart : full.size();
    if (srcEnd <= srcStart) continue;  // empty pass: skip
    out.push_back({markers[i].name, full.substr(srcStart, srcEnd - srcStart)});
  }
  return out;
}

}  // namespace ns
