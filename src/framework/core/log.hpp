// ---------------------------------------------------------------------------
// Log - tiny tagged logging for the framework (GL-free).
// Levels: error / warn / info / debug. Tags let subsystems filter noise.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <functional>
#include <string>

namespace ns {

namespace Log {

enum Level { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

inline Level& level() {
  static Level l = kInfo;
  return l;
}
inline void setLevel(Level l) { level() = l; }

/** optional capture hook: receives every emitted line ("[INF][TAG] msg");
 *  used by --check-hotreload to verify the reload log pair programmatically. */
using Sink = std::function<void(const std::string& line)>;
inline Sink& sink() {
  static Sink s;
  return s;
}
inline void setSink(Sink s) { sink() = std::move(s); }

inline void write(Level lv, const char* tag, const std::string& msg) {
  if (lv > level()) return;
  const char* n = lv == kError ? "ERR" : lv == kWarn ? "WRN" : lv == kInfo ? "INF" : "DBG";
  const std::string line = std::string("[") + n + "][" + tag + "] " + msg;
  std::fprintf(stderr, "%s\n", line.c_str());
  if (sink()) sink()(line);
}

inline void error(const char* tag, const std::string& msg) { write(kError, tag, msg); }
inline void warn(const char* tag, const std::string& msg) { write(kWarn, tag, msg); }
inline void info(const char* tag, const std::string& msg) { write(kInfo, tag, msg); }
inline void debug(const char* tag, const std::string& msg) { write(kDebug, tag, msg); }

}  // namespace Log

}  // namespace ns
