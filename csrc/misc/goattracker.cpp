// SPDX-License-Identifier: GPL-3.0-or-later
// GoatTracker songs (.sng): the editor's own files, not C64 programs.
//
// The song is played by the editor's play routine, which is the C64 play routine in C,
// and reSID. Songs of GoatTracker 1 ("GTS!") are played by version 1's routine, all
// others ("GTS5", "GTS4", "GTS3", and "GTS2" converted as the editor converts it) by
// version 2's.
//
// What a song doesn't say about itself: how many times per frame it is to be called
// (the "multiplier"), the SID model, and the editor's options at the time (the ADSR of
// the hard restart, and whether pulse and effects are skipped on a note's first frame).
// The editor takes those from its command line. Here they come, in this order, from
//   - the environment (BBMP_GT_MULTIPLIER, _MODEL, _ADPARAM, _OPTIMIZE, ...), for tests
//   - gt_songs.inc, a list of the songs on Modland whose settings are known, because the
//     same song is in the High Voltage SID Collection as a C64 program and the settings
//     listed make this player write the same values to the SID as that program does
//   - the file's name, where Modland has them ("... 2x-speed", "_x2", "_6581", "_ffff",
//     "_po_ro")
//   - the editor's defaults.
//
// A song never ends by itself: its order lists end with a jump. It is over when the
// three voices are at a place, and in a state, in which they have been before.

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "common.h"
#include "gt_engine.h"

namespace misc {

namespace {

constexpr double kPalFrames = 985248.0 / (63 * 312);    // 50.12 frames per second
constexpr long long kLongest = 10 * 60;                 // seconds: no song is taken to be longer

struct Walker {
  const std::vector<uint8_t>& d;
  size_t pos = 0;
  bool ok = true;
  unsigned byte() {
    if (pos >= d.size()) { ok = false; return 0; }
    return d[pos++];
  }
  void skip(size_t n) {
    if (pos + n > d.size()) ok = false;
    else pos += n;
  }
};

// Whether the file fits into the editor's arrays; the editor's loader doesn't ask.
std::string checkGt2(const std::vector<uint8_t>& d, int tables) {
  Walker w{d};
  w.skip(4 + 3 * 32);
  const unsigned songs = w.byte();
  if (songs > 32) return "more songs than GoatTracker has";
  for (unsigned s = 0; s < songs; s++)
    for (int c = 0; c < 3; c++) w.skip(w.byte() + 1);
  const unsigned instruments = w.byte();
  if (instruments > 63) return "more instruments than GoatTracker has";
  w.skip(instruments * 25);
  for (int t = 0; t < tables; t++) w.skip(2 * w.byte());
  const unsigned patterns = w.byte();
  if (patterns > 208) return "more patterns than GoatTracker has";
  for (unsigned p = 0; p < patterns; p++) {
    const unsigned rows = w.byte();
    if (rows > 129) return "a pattern longer than GoatTracker's";
    w.skip(rows * 4);
  }
  return w.ok ? "" : "the file is cut short";
}

struct Hints {
  GtOptions options{1, 0x0f00, 1, 1, 1, 2, 1};
  bool model8580 = false;
  std::string said;          // what the name gave, for the header
  std::string known;         // what the list of known songs gave
};

enum { kPulse = 1, kRealtime = 2, kFineVibrato = 4, kOneFrameRestart = 8 };

struct Known {
  uint64_t hash;             // FNV-1a, 64 bits, of the whole file
  unsigned char multiplier;
  unsigned short model;      // 6581 or 8580
  unsigned short adparam;
  unsigned char flags;
  const char* checked;       // how well the SID registers agree with the C64 program's
};

const Known kKnown[] = {
#include "gt_songs.inc"
    {0, 0, 0, 0, 0, nullptr}};

uint64_t fnv1a(const std::vector<uint8_t>& d) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (uint8_t b : d) {
    h ^= b;
    h *= 0x100000001b3ULL;
  }
  return h;
}

bool digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

Hints settings(const char* path, const std::vector<uint8_t>& file) {
  Hints h;
  std::string name = path;
  const auto slash = name.rfind('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  const auto dot = name.rfind('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  std::vector<std::string> tokens;
  std::string token;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      token += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else if (!token.empty()) {
      tokens.push_back(token);
      token.clear();
    }
  }
  if (!token.empty()) tokens.push_back(token);

  bool flags = false, fv = false, po = false, ro = false;
  auto say = [&](const std::string& s) { h.said += (h.said.empty() ? "" : ", ") + s; };
  for (size_t i = 0; i < tokens.size(); i++) {
    const std::string& t = tokens[i];
    // "2x", "x2", and "2x" before "speed"; never a lone number
    std::string number;
    if (t.size() >= 2 && t.back() == 'x' && digits(t.substr(0, t.size() - 1))) number = t.substr(0, t.size() - 1);
    if (t.size() >= 2 && t.front() == 'x' && digits(t.substr(1))) number = t.substr(1);
    if (!number.empty() && number.size() <= 2 && i > 0) {
      const int n = std::atoi(number.c_str());
      if (n >= 2 && n <= 16) {
        h.options.multiplier = n;
        say(std::to_string(n) + "x speed");
        continue;
      }
    }
    if (i == 0) continue;                       // the first word is the song's name
    if (t == "6581") { h.model8580 = false; say("6581"); continue; }
    if (t == "8580") { h.model8580 = true; say("8580"); continue; }
    if (t == "fv") { fv = flags = true; continue; }
    if (t == "po") { po = flags = true; continue; }
    if (t == "ro") { ro = flags = true; continue; }
    if (t.size() == 4 && t.find_first_not_of("0123456789abcdef") == std::string::npos) {
      const bool digit = t.find_first_of("0123456789") != std::string::npos;
      const bool letter = t.find_first_of("abcdef") != std::string::npos;
      if ((digit && letter) || t == "ffff" || t == "0000") {
        h.options.adparam = static_cast<unsigned>(std::strtoul(t.c_str(), nullptr, 16));
        say("hard restart " + t);
        flags = true;
        continue;
      }
    }
  }
  if (flags) {
    // a name that lists the editor's options lists the ones that were on
    h.options.finevibrato = fv;
    h.options.optimizepulse = po;
    h.options.optimizerealtime = ro;
    say(std::string("fine vibrato ") + (fv ? "on" : "off"));
    say(std::string("pulse optimization ") + (po ? "on" : "off"));
    say(std::string("realtime optimization ") + (ro ? "on" : "off"));
  }
  if (std::getenv("BBMP_GT_HASH")) std::fprintf(stderr, "0x%016llxULL\n", static_cast<unsigned long long>(fnv1a(file)));
  const uint64_t hash = fnv1a(file);
  for (const Known* k = kKnown; k->checked && !std::getenv("BBMP_GT_UNKNOWN"); k++) {
    if (k->hash != hash) continue;
    h.options.multiplier = k->multiplier;
    h.model8580 = k->model == 8580;
    h.options.adparam = k->adparam;
    h.options.optimizepulse = (k->flags & kPulse) != 0;
    h.options.optimizerealtime = (k->flags & kRealtime) != 0;
    h.options.finevibrato = (k->flags & kFineVibrato) != 0;
    h.options.hardrestart = (k->flags & kOneFrameRestart) ? 1 : 2;
    h.known = k->checked;
    h.said.clear();
    break;
  }
  if (const char* e = std::getenv("BBMP_GT_MULTIPLIER")) {
    const int n = std::atoi(e);
    if (n >= 0 && n <= 16) h.options.multiplier = n;
  }
  if (const char* e = std::getenv("BBMP_GT_MODEL")) h.model8580 = std::atoi(e) == 8580;
  if (const char* e = std::getenv("BBMP_GT_ADPARAM"))
    h.options.adparam = static_cast<unsigned>(std::strtoul(e, nullptr, 16)) & 0xffff;
  if (const char* e = std::getenv("BBMP_GT_FINEVIBRATO")) h.options.finevibrato = std::atoi(e);
  if (const char* e = std::getenv("BBMP_GT_HARDRESTART")) h.options.hardrestart = std::atoi(e);
  if (const char* e = std::getenv("BBMP_GT_OLDWAVES")) h.options.oldwaves = std::atoi(e);
  if (const char* e = std::getenv("BBMP_GT_OPTIMIZE")) {
    h.options.optimizepulse = std::strchr(e, 'p') != nullptr;
    h.options.optimizerealtime = std::strchr(e, 'r') != nullptr;
  }
  return h;
}

struct Engine {
  int version;               // 1, 2, or 3 for version 1 stereo
  void (*start)(int);
  int (*tick)(unsigned char*);
  void (*state)(GtState*);
  int (*wrapped)();
  const char* (*instrument)(int);
};

struct StateHash {
  size_t operator()(const std::string& s) const { return std::hash<std::string>()(s); }
};

// Play without sound until the song repeats. Returns the number of calls of the play
// routine that make up the song played once; 0 when it stops at once.
//
// Voices may have tempos of their own, and order lists of different lengths. Then the
// three come back to where they were together late or never, and the song is taken to
// be over when each voice has been through its order list once.
long long measure(const Engine& e, int song, double callsPerSecond, std::string& how) {
  e.start(song);
  std::unordered_map<std::string, long long> seen;
  unsigned char regs[64];
  GtState state;
  const long long most = static_cast<long long>(kLongest * callsPerSecond);
  long long allWrapped = 0;
  // for comparing with the C64 play routine: the registers after every call
  FILE* dump = nullptr;
  if (const char* to = std::getenv("BBMP_GT_REGDUMP")) dump = std::fopen(to, "w");
  struct Closer {
    FILE* f;
    ~Closer() { if (f) std::fclose(f); }
  } closer{dump};
  for (long long n = 0; n < most; n++) {
    e.state(&state);
    const std::string key(reinterpret_cast<const char*>(state.bytes), sizeof(state.bytes));
    if (seen.count(key)) {
      how = "the song repeats";
      return n;
    }
    seen.emplace(key, n);
    if (!e.tick(regs)) {
      how = "the song stops";
      return n;
    }
    if (dump) {
      for (int r = 0; r < (e.version == 3 ? 50 : 25); r++) std::fprintf(dump, "%02x", regs[r]);
      std::fputc('\n', dump);
    }
    if (!allWrapped && e.wrapped() == (e.version == 3 ? 63 : 7)) allWrapped = n + 1;
  }
  if (allWrapped) {
    how = "the voices don't repeat together; every voice has played its list once";
    return allWrapped;
  }
  how = "no end found";
  return most;
}

}  // namespace

int playGoatTracker(const char* path, int track, int rate, long long startMs) {
  const std::vector<uint8_t> file = readFile(path);
  if (file.size() < 4 + 3 * 32 + 1 || std::memcmp(file.data(), "GTS", 3) != 0)
    return fail("not a GoatTracker song");
  const char version = static_cast<char>(file[3]);
  Hints hints = settings(path, file);
  GtSong song{};
  Engine engine{};
  std::string type;
  if (version == '!') {
    // the stereo editor's songs have six order lists per song, and nothing that says so
    if (gt1w_load(file.data(), file.size(), &hints.options, &song)) {
      engine = {1, gt1w_start, gt1w_tick, gt1w_state, gt1w_wrapped, gt1w_instrument};
      type = "GoatTracker 1 (GTS!)";
    } else if (gt1sw_load(file.data(), file.size(), &hints.options, &song)) {
      engine = {3, gt1sw_start, gt1sw_tick, gt1sw_state, gt1sw_wrapped, gt1sw_instrument};
      type = "GoatTracker 1 Stereo (GTS!)";
    } else {
      return fail("the GoatTracker 1 song is damaged");
    }
  } else if (version == '2' || version == '3' || version == '4' || version == '5') {
    const std::string why = checkGt2(file, version == '2' ? 3 : 4);
    if (!why.empty()) return fail("GoatTracker song: " + why);
    if (!gt2w_load(file.data(), file.size(), &hints.options, &song))
      return fail("GoatTracker doesn't load the song");
    engine = {2, gt2w_start, gt2w_tick, gt2w_state, gt2w_wrapped, gt2w_instrument};
    type = std::string("GoatTracker 2 (GTS") + version + ")";
  } else {
    return fail(std::string("a GoatTracker song of an unknown version: GTS") + version);
  }
  if (song.songs < 1) return fail("the GoatTracker song is empty");
  if (track < 0 || track >= song.songs) track = 0;

  const unsigned multiplier = hints.options.multiplier;
  const double callsPerSecond = multiplier ? kPalFrames * multiplier : kPalFrames / 2;
  std::string how;
  const long long calls = measure(engine, track, callsPerSecond, how);
  if (calls < 1) return fail("the GoatTracker song stops at once");

  Info info;
  info.tracks = song.songs;
  info.track = track;
  info.type = type;
  info.title = fromLatin1(field(song.name, 32));
  info.author = fromLatin1(field(song.author, 32));
  info.comment = fromLatin1(field(song.copyright, 32));
  info.durationMs = static_cast<long long>(calls / callsPerSecond * 1000.0);
  info.facts.emplace_back("SID", hints.model8580 ? "8580" : "6581");
  info.facts.emplace_back("speed", multiplier ? std::to_string(multiplier) + "x" : "25 Hz");
  {
    char text[16];
    std::snprintf(text, sizeof text, "%04x", hints.options.adparam);
    info.facts.emplace_back("hard restart", text);
  }
  if (engine.version == 2)
    info.facts.emplace_back("skipped on a note's first frame",
                            std::string(hints.options.optimizepulse ? "pulse " : "") +
                                (hints.options.optimizerealtime ? "effects" : ""));
  info.facts.emplace_back("settings", !hints.known.empty() ? "known: " + hints.known
                                      : !hints.said.empty() ? "from the file's name: " + hints.said
                                      : "the editor's defaults");
  info.facts.emplace_back("end", how);
  for (int i = 1; i <= song.instruments; i++) info.names.push_back(fromLatin1(field(engine.instrument(i), 16)));

  gtw_sid_init(rate, hints.model8580, 0);
  engine.start(track);
  long long called = 0;
  double owed = 0;                          // samples the calls so far stand for
  long long made = 0;                       // samples rendered for them
  std::vector<short> left, right;           // one call's samples, rendered in one go
  size_t taken = 0;
  unsigned char regs[64] = {0};
  // returns the frames written, less than asked for at the end
  auto render = [&](int16_t* out, int frames) {
    int done = 0;
    while (done < frames) {
      if (taken >= left.size()) {
        if (called >= calls) break;
        engine.tick(regs);
        called++;
        owed += rate / callsPerSecond;
        const int n = static_cast<int>(static_cast<long long>(owed) - made);
        made += n;
        left.assign(n, 0);
        taken = 0;
        if (n > 0) {
          if (engine.version == 2) gtw_sid_render(regs, left.data(), n);
          else gtw_sid_render1(0, regs, left.data(), n);
          if (engine.version == 3) {            // the second SID is the right channel
            right.assign(n, 0);
            gtw_sid_render1(1, regs + 25, right.data(), n);
          }
        }
        continue;
      }
      out[2 * done] = left[taken];
      out[2 * done + 1] = engine.version == 3 ? right[taken] : left[taken];
      taken++;
      done++;
    }
    return done;
  };
  std::vector<int16_t> stereo(2 * 4096);
  long long toSkip = startMs * rate / 1000;
  while (toSkip > 0) {
    const int n = toSkip > 4096 ? 4096 : static_cast<int>(toSkip);
    if (render(stereo.data(), n) < n) break;
    toSkip -= n;
  }
  header(info);
  for (;;) {
    const int n = render(stereo.data(), 4096);
    if (n > 0 && !emit(stereo.data(), n)) break;
    if (n < 4096) break;
  }
  return 0;
}

}  // namespace misc
