// ---------------------------------------------------------------------------
// NULL SECTOR - master show schedule (port of src/shared/schedule.ts)
// Single source of truth for BPM, section layout and intensity.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <string>
#include <vector>

namespace ns {

// the real track (nullsectordemoengine.wav) runs at ~216 BPM; keep the beat grid
// in lockstep so kick/beat-synced visuals land on the actual drum hits
inline constexpr float BPM = 216.0f;
/** seconds per beat */
inline constexpr float BEAT = 60.0f / BPM;
/** seconds per bar (4 beats) */
inline constexpr float BAR = BEAT * 4.0f;

// narrative chapters: SILENCE .. TRANSCENDENCE
inline const std::array<const char*, 6> CHAPTERS = {"SILENCE", "DISCOVERY", "ACTIVATION", "EXPANSION", "OVERLOAD", "TRANSCENDENCE"};

// ---------------------------------------------------------------------------
// Musical chord progression: one distinct hue per bar, in palVoid space.
// A 16-chord harmonic loop cycles every 16 bars; a slow phrase drift walks
// the loop forward across the show so the overall mood arcs from purple
// (awakening) through blue/cyan (expansion) toward magenta (climax).
// musicHue = CHORD_HUES[bar % 16] + phrase * PHRASE_DRIFT, wrapped to 0..1.
// ---------------------------------------------------------------------------
inline constexpr float CHORD_PHRASE_DRIFT = 0.055f;  // mood arc per 16-bar phrase
inline const std::array<float, 16> CHORD_HUES = {
  0.00f, 0.04f, 0.10f, 0.17f,   // purple -> indigo  (awakening)
  0.26f, 0.34f, 0.42f, 0.50f,   // -> blue            (discovery)
  0.58f, 0.65f, 0.71f, 0.78f,   // -> cyan            (expansion)
  0.86f, 0.92f, 0.97f, 0.02f,   // -> magenta -> wrap  (climax)
};

/** hue (0..1, palVoid space) of the chord for an absolute bar index */
inline float chordHue(int bar) {
  const int k = ((bar % 16) + 16) % 16;
  const float phrase = (float)(bar / 16);  // trunc toward zero: bar 0..15 -> 0
  float h = CHORD_HUES[k] + phrase * CHORD_PHRASE_DRIFT;
  h -= (float)(int)h;  // wrap to [0,1)
  return h;
}

struct SectionDef {
  const char* name;
  int bars;
  float intensity;
  int chapter;
};

inline const std::array<SectionDef, 16> SECTIONS = {{
  // intro is 60 bars (~66.7s @ 216 BPM) to cover the three music-timed
  // phases:
  //   0:00-0:20 Awakening  (black, particles, lone scanner, thin grid, LEDs)
  //   0:21-0:48 Communication (boot log, diagnostics, node graphs, FFT, hex)
  //   0:49-1:06 Build-up   (scanner accelerates, streams, ripples, ghost)
  {"intro", 60, 0.12f, 0},
  {"tunnel", 8, 0.45f, 1},
  {"cathedral", 9, 0.55f, 1},    {"neuralnet", 14, 0.62f, 2},
  {"infinitemachine", 10, 0.68f, 2},
  {"ghostformation", 12, 0.72f, 3},
  {"voxel", 10, 0.88f, 3},
  {"logo", 17, 1.0f, 4},
  {"reprise", 20, 0.96f, 4},
  {"greetings", 11, 0.3f, 5},
  // The final sign-off finishes its fade at ~17.2s, so leave one full bar
  // after it before the automatic shutdown.
  {"credits", 13, 0.22f, 5},
  // --- the extension: the show now runs the FULL track (330s, 297 bars) -----
  // The real track breaks down at ~204s (where the old show ended), hits a
  // near-silent drop at ~236s, rebuilds from ~240s, then runs a max-intensity
  // climax from ~300s to the abrupt end. These five sections map that arc,
  // reusing the dormant cathedral deconstruction mode and the reprise shaders
  // (all secT-normalised, so re-timing stays in sync):
  {"deconstruction", 28, 0.25f, 4},   // breakdown: the cathedral falls (ghost)
  {"stillness", 4, 0.08f, 5},         // near-silent drop: the ghost fully forms
  {"reascension", 28, 0.70f, 5},      // rebuild: the machine revs up again
  {"synapse", 26, 0.80f, 5},          // climb: the network as the ghost's mind
  {"convergence", 27, 1.0f, 5},       // climax: the title returns, blazing
}};

struct SectionInfo {
  int index = 0;
  std::string name;
  float startBeat = 0, endBeat = 0;
  float startSec = 0, endSec = 0, duration = 0;
  float intensity = 0;
  int chapter = 0;
};

inline std::vector<SectionInfo> buildSections() {
  std::vector<SectionInfo> out;
  out.reserve(SECTIONS.size());
  float acc = 0;
  for (int i = 0; i < (int)SECTIONS.size(); i++) {
    const SectionDef& s = SECTIONS[i];
    const float startBeat = acc;
    const float endBeat = acc + s.bars * 4;
    acc = endBeat;
    SectionInfo si;
    si.index = i;
    si.name = s.name;
    si.startBeat = startBeat;
    si.endBeat = endBeat;
    si.startSec = startBeat * BEAT;
    si.endSec = endBeat * BEAT;
    si.duration = s.bars * 4 * BEAT;
    si.intensity = s.intensity;
    si.chapter = s.chapter;
    out.push_back(si);
  }
  return out;
}

inline const std::vector<SectionInfo>& SECTION_INFO() {
  static const std::vector<SectionInfo> info = buildSections();
  return info;
}

inline float TOTAL_SECONDS() {
  const auto& v = SECTION_INFO();
  return v.empty() ? 0 : v.back().endSec;
}

/** index of the section whose time range contains t (clamped to last) */
inline int sectionIndexAt(float t) {
  int idx = 0;
  const auto& v = SECTION_INFO();
  for (int i = 0; i < (int)v.size(); i++) {
    if (t >= v[i].startSec) idx = i;
    else break;
  }
  return idx;
}

/** the section whose time range contains t */
inline const SectionInfo& sectionAt(float t) {
  return SECTION_INFO()[sectionIndexAt(t)];
}

/** RGB hints per section */
inline std::array<float, 3> sectionTint(const std::string& name) {
  static const struct { const char* n; float r, g, b; } tints[] = {
    {"intro", 0.3f, 0.2f, 0.6f},
    {"tunnel", 0.6f, 0.25f, 1.0f},
    {"cathedral", 0.1f, 0.7f, 1.0f},
    {"neuralnet", 0.15f, 0.9f, 0.7f},
    {"infinitemachine", 0.9f, 0.3f, 0.5f},
    {"ghostformation", 0.2f, 0.7f, 1.0f},
    {"voxel", 0.5f, 0.3f, 1.0f},
    {"logo", 0.85f, 0.35f, 1.0f},
    {"reprise", 0.9f, 0.2f, 0.9f},
    {"greetings", 0.9f, 0.6f, 0.2f},
    {"credits", 0.4f, 0.7f, 1.0f},
    {"deconstruction", 0.9f, 0.2f, 0.9f},
    {"stillness", 0.2f, 0.7f, 1.0f},
    {"reascension", 0.9f, 0.3f, 0.5f},
    {"synapse", 0.15f, 0.9f, 0.7f},
    {"convergence", 0.85f, 0.35f, 1.0f},
  };
  for (const auto& t : tints) if (name == t.n) return {t.r, t.g, t.b};
  return {0.58f, 0.65f, 0.72f};
}

}  // namespace ns
