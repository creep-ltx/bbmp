// SPDX-License-Identifier: GPL-2.0-or-later
// The "wsr" engine: Bandai WonderSwan music. A .wsr is a piece of a game's ROM with
// its sound driver, 64 KiB or a multiple, that ends in a footer of 32 bytes: "WSRF",
// a version, the number of the first song, and the last 16 bytes of the ROM with the
// jump the CPU starts at. The song number is handed over in the register AX.
//
// It is played by Mednafen's WonderSwan emulation, in the form foo_input_wsr has it
// (without video), which knows WSR itself.
//
// A .wsr doesn't say which song numbers it has, nor how long a song is. So:
//  - every number from 0 to 255 is started and listened to for a few seconds; the
//    ones that make a sound are the tracks (as rePlayer does it), and numbers that
//    do exactly the same as an earlier number are left out. Track 0 is the song the
//    footer names, the numbers below it come after the highest;
//  - the song is run through once without being played, while 75 times a second
//    (once per frame of the display) it is noted what the sound chip is set to.
//    Where that starts to repeat is the loop, where the sound stops is the end.

#include "wswan/wswan.h"
#include "wswan/gfx.h"
#include "wswan/interrupt.h"
#include "wswan/memory.h"
#include "wswan/sound.h"
#include "wswan/v30mz.h"

#include "engine.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {
bool g_skip_idle = true;
unsigned long long g_skipped = 0, g_run = 0;
}  // namespace

// The rips spend most of their time waiting for the next interrupt, 97 to 99 % of it
// in most of them, and emulating that is what takes the time. gfx.cpp is compiled
// with v30mz_execute renamed to v30mz_execute_hooked, so that its calls arrive here,
// where the waiting is skipped. Interrupts are raised between those calls only,
// which is why nothing can end the waiting within one of them.
namespace MDFN_IEN_WSWAN {
static inline uint8 peek(uint32 a) { return WSwan_readmem20(a & 0xFFFFF); }

// "cmp byte [address], value / jne or je back to the cmp", with the byte as it is
static bool waits_for_byte(uint32 cmp) {
  if (peek(cmp) != 0x80 || peek(cmp + 1) != 0x3E || peek(cmp + 6) != 0xF9) return false;
  const uint8 jump = peek(cmp + 5);
  if (jump != 0x75 && jump != 0x74) return false;
  const uint32 address = peek(cmp + 2) | (peek(cmp + 3) << 8);
  const uint8 now = peek((v30mz_get_reg(NEC_DS0) << 4) + address);
  return (now != peek(cmp + 4)) == (jump == 0x75);
}

const uint32 kNoLoop = 0xFFFFFFFF;

// Is the CPU in a loop that does nothing until an interrupt comes? Returns where
// the loop begins, or kNoLoop.
static uint32 waiting() {
  const uint32 pc = (v30mz_get_reg(NEC_PS) << 4) + v30mz_get_reg(NEC_PC);
  const uint8 a = peek(pc), b = peek(pc + 1);
  if (a == 0xEB && b == 0xFE) return pc;  // jmp $
  if (a == 0x80) return waits_for_byte(pc) ? pc : kNoLoop;
  if ((a == 0x75 || a == 0x74) && b == 0xF9) return waits_for_byte(pc - 5) ? pc - 5 : kNoLoop;
  return kNoLoop;
}

// how many cycles the loop last met takes to go round once
static uint32 g_loop = kNoLoop;
static int g_round = 0;

// Emulates exactly one instruction; false when the piece is too short for that.
static inline bool one_instruction(int& left) {
  const int need = 1 - v30mz_ICount;  // what it takes to make v30mz_execute go on
  if (need < 1 || need > left) return false;
  v30mz_execute(need);
  left -= need;
  return true;
}

void v30mz_execute_hooked(int cycles) {
  // Not waiting: as Mednafen does it, in one piece. (Pieces are not quite the same:
  // an instruction with a repeat prefix lets go at the end of a piece.)
  uint32 loop;
  if (!g_skip_idle || (loop = waiting()) == kNoLoop) {
    g_run += cycles;
    v30mz_execute(cycles);
    return;
  }
  // Waiting, but an interrupt may have come since the last call. An instruction of
  // the loop is emulated, which takes the interrupt if there is one. If there is
  // none, as many whole rounds of the loop as fit are left out, and the rest is
  // emulated, which ends exactly where Mednafen would have ended.
  g_run += cycles;
  int left = cycles;
  if (loop != g_loop) {
    // a loop not met before: how long does a round take?
    const uint32 home = (v30mz_get_reg(NEC_PS) << 4) + v30mz_get_reg(NEC_PC);
    uint32 first = 0;
    bool been_home = false;
    while (one_instruction(left)) {
      if (waiting() != loop) break;  // woken
      if ((v30mz_get_reg(NEC_PS) << 4) + v30mz_get_reg(NEC_PC) != home) continue;
      if (!been_home) {
        been_home = true;
        first = v30mz_timestamp;
        continue;
      }
      g_round = static_cast<int>(v30mz_timestamp - first);
      if (g_round > 0) g_loop = loop;
      break;
    }
  } else if (one_instruction(left) && waiting() == loop) {
    const int rounds = left / g_round;
    v30mz_timestamp += rounds * g_round;
    g_skipped += rounds * g_round;
    left -= rounds * g_round;
  }
  if (left > 0) v30mz_execute(left);
}
}  // namespace MDFN_IEN_WSWAN

namespace {

using namespace MDFN_IEN_WSWAN;

const double kFramesPerSecond = 3072000.0 / (159 * 256);  // 75.47
const int kScanSeconds = 4;       // how long a song number may take to make a sound
const double kScanMoreSeconds = 1.5;  // and how much of it is compared with the other numbers
const int kLookSeconds = 600;     // how far a song is followed to find its end
const int kDefaultSeconds = 180;  // how long a song plays whose end wasn't found
const int kFadeSeconds = 4;       // the fade after a loop
const double kSilenceSeconds = 4.0;  // this much silence is the end of a song
const int kQuiet = 16;            // samples below this are silence

// What the sound chip is set to at the end of a frame: per channel a number for the
// note (pitch, wave form, kind of sound; 0 when the channel is silent) and the volume.
struct State {
  uint64_t note[4];
  uint8_t volume[4];
};

struct Found {
  enum Kind { kNothing, kSilence, kLoop, kStill } kind = kNothing;
  double end = 0;  // seconds: where the song stops, or where the loop has played once
  double loop_start = 0, loop_length = 0;
};

// ---- finding the loop ----
//
// Drivers don't repeat themselves exactly. Many count the tempo in fractions of a
// frame, so that a note of the second time round comes a frame earlier or later than
// the first time, and some have a vibrato or tremolo that runs on regardless of the
// song. So the comparison of a frame with the frame one loop later allows a few
// frames of shift, per channel, and a small share of frames that differ.

const int kShift = 4;                // frames a channel may be early or late
const double kNoteRate = 0.97;       // share of frames whose notes have to match
const double kVolumeRate = 0.90;     // the same for the volumes
const double kWindowSeconds = 2.0;   // nowhere may more than kWindowBad of such a
const double kWindowBad = 0.35;      //   stretch differ in its notes
const double kWindowBadVolume = 0.6; //   or in its volumes
const double kSeenSeconds = 45.0;    // so much of a long loop has to be seen twice
const double kSteadySeconds = 20.0;  // and a short loop has to go on for so long

class LoopFinder {
 public:
  explicit LoopFinder(const std::vector<State>& s) : s_(s), n_(static_cast<int>(s.size())) {}

  bool find(Found& found) {
    if (n_ < static_cast<int>(kSteadySeconds * kFramesPerSecond) + 2 * kShift) return false;
    const std::vector<int> lags = candidates();
    if (debug_on()) {
      std::fprintf(stderr, "wsr: %d frames, %zu lags to look at:", n_, lags.size());
      for (size_t i = 0; i < lags.size() && i < 12; ++i) std::fprintf(stderr, " %.2f", lags[i] / kFramesPerSecond);
      std::fprintf(stderr, "\n");
    }
    for (int lag : lags) {
      int start;
      if (!holds(lag, start)) continue;
      found.kind = Found::kLoop;
      found.loop_start = start / kFramesPerSecond;
      found.loop_length = lag / kFramesPerSecond;
      found.end = found.loop_start + found.loop_length;
      return true;
    }
    return false;
  }

 private:
  static uint64_t mix(uint64_t x, uint64_t v) { return (x ^ v) * 1099511628211ull; }

  uint64_t all_notes(int f) const {
    uint64_t x = 1469598103934665603ull;
    for (int c = 0; c < 4; ++c) x = mix(x, s_[f].note[c]);
    return x;
  }

  // Lags worth a closer look: frames near the end at which a note changes are
  // looked up among the earlier frames with the same change. A lag that many of
  // them agree on is a candidate.
  std::vector<int> candidates() const {
    std::vector<uint64_t> notes(n_);
    for (int f = 0; f < n_; ++f) notes[f] = all_notes(f);
    std::vector<std::pair<uint64_t, int>> changes;  // the change and its frame
    for (int f = 1; f < n_; ++f)
      if (notes[f] != notes[f - 1]) changes.push_back({mix(mix(7, notes[f - 1]), notes[f]), f});
    std::vector<int> anchors;
    const int from = n_ - n_ / 5;
    for (const auto& c : changes)
      if (c.second >= from) anchors.push_back(c.second);
    if (anchors.size() < 4) return {};
    const size_t kAnchors = 32;
    if (anchors.size() > kAnchors) {
      std::vector<int> some;
      for (size_t i = 0; i < kAnchors; ++i) some.push_back(anchors[i * anchors.size() / kAnchors]);
      anchors.swap(some);
    }
    std::vector<std::pair<uint64_t, int>> sorted = changes;
    std::sort(sorted.begin(), sorted.end());
    std::vector<int> votes(n_ + 1, 0);
    for (int a : anchors) {
      const uint64_t key = mix(mix(7, notes[a - 1]), notes[a]);
      auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(key, 0));
      for (; it != sorted.end() && it->first == key && it->second < a; ++it) {
        const int lag = a - it->second;
        if (lag >= 8) ++votes[lag];
      }
    }
    std::vector<int> lags;
    const int need = std::max(2, static_cast<int>(anchors.size() * 2 / 5));
    for (int lag = 8; lag <= n_ - 1; ++lag) {
      if (!votes[lag]) continue;
      int sum = 0, best = 0;
      for (int o = -2; o <= 2; ++o)
        if (lag + o >= 0 && lag + o <= n_) { sum += votes[lag + o]; best = std::max(best, votes[lag + o]); }
      if (sum >= need && votes[lag] == best) lags.push_back(lag);
    }
    return lags;
  }

  bool same_notes(int f, int lag) const {
    for (int c = 0; c < 4; ++c) {
      const uint64_t want = s_[f].note[c];
      bool ok = false;
      for (int o = -kShift; o <= kShift && !ok; ++o) ok = s_[f + lag + o].note[c] == want;
      if (!ok) return false;
    }
    return true;
  }

  bool same_volumes(int f, int lag) const {
    for (int c = 0; c < 4; ++c) {
      const uint8_t want = s_[f].volume[c];
      bool ok = false;
      for (int o = -kShift; o <= kShift && !ok; ++o) ok = s_[f + lag + o].volume[c] == want;
      if (!ok) return false;
    }
    return true;
  }

  // Does the song repeat itself after <lag> frames, up to the end of what is known
  // of it? If so, <start> is the frame from which on it does.
  bool holds(int lag, int& start) const {
    const int n = n_ - lag - kShift;  // frames that have a partner
    const int need = static_cast<int>(
        std::max(std::min<double>(lag, kSeenSeconds * kFramesPerSecond), kSteadySeconds * kFramesPerSecond));
    if (n < need + kShift) return false;
    // a quick look first: most candidates fail at once
    int bad = 0;
    for (int i = 0; i < 64; ++i) {
      const int f = n - 1 - i * (need - 1) / 64;
      if (f >= kShift && !same_notes(f, lag)) ++bad;
    }
    if (debug_on()) std::fprintf(stderr, "wsr:   lag %.2f: quick look %d of 64 differ\n", lag / kFramesPerSecond, bad);
    if (bad > 16) return false;

    // Backwards from the end, until a stretch of the song differs too much.
    const int window = static_cast<int>(kWindowSeconds * kFramesPerSecond);
    std::vector<uint8_t> notes(n, 0), volumes(n, 0);
    int notes_in_window = 0, volumes_in_window = 0, f = n - 1;
    for (; f >= kShift; --f) {
      notes[f] = !same_notes(f, lag);
      volumes[f] = !same_volumes(f, lag);
      notes_in_window += notes[f];
      volumes_in_window += volumes[f];
      if (f + window < n) {
        notes_in_window -= notes[f + window];
        volumes_in_window -= volumes[f + window];
      }
      if (notes_in_window > kWindowBad * window || volumes_in_window > kWindowBadVolume * window) break;
    }
    // f is where it stopped being the same; the loop starts after the last
    // difference of that stretch
    start = 0;
    if (f >= kShift) {
      int last = f;
      for (int k = f; k < n && k < f + window; ++k)
        if (notes[k] || volumes[k]) last = k;
      start = last + 1;
    }
    const int span = n - start;
    if (debug_on())
      std::fprintf(stderr, "wsr:   lag %.2f: the same from %.2f s on, for %.2f s, %.2f s needed\n",
                   lag / kFramesPerSecond, start / kFramesPerSecond, span / kFramesPerSecond, need / kFramesPerSecond);
    if (span < need) return false;
    long notes_bad = 0, volumes_bad = 0;
    for (int k = std::max(start, kShift); k < n; ++k) {
      notes_bad += notes[k];
      volumes_bad += volumes[k];
    }
    return notes_bad <= (1 - kNoteRate) * span && volumes_bad <= (1 - kVolumeRate) * span;
  }

  const std::vector<State>& s_;
  const int n_;
};

// ---- the engine ----

class WsrEngine : public Engine {
 public:
  ~WsrEngine() override {
    if (loaded_) CloseGame();
  }

  bool open(const std::vector<uint8_t>& file, int track, int rate, SongInfo& info, std::string& error) override {
    if (file.size() < 0x10000 || (file.size() & 0xFFFF) || file.size() > (64u << 20)) {
      error = "not a WSR: the size isn't a multiple of 64 KiB";
      return false;
    }
    const uint8_t* footer = file.data() + file.size() - 0x20;
    if (std::memcmp(footer, "WSRF", 4)) { error = "not a WSR: no WSRF footer"; return false; }
    // the CPU starts at the footer's 16th byte; an empty one can't be a ROM
    bool blank = true;
    for (int i = 0x10; i < 0x16; ++i) blank = blank && (footer[i] == 0x00 || footer[i] == 0xFF);
    if (blank) { error = "not a WSR: nothing to start at"; return false; }
    g_skip_idle = !std::getenv("BBMP_CONSOLE_NO_SKIP");
    if (const char* seconds = std::getenv("BBMP_CONSOLE_SCAN_SECONDS")) scan_seconds_ = std::atoi(seconds);
    // Mednafen patches the ROM of one game (Detective Conan), which it knows by the
    // checksum, at an address a megabyte up. A rip of that game is smaller, and the
    // patch would be written to where no ROM is: not with this checksum, then.
    std::vector<uint8_t> rom(file);
    uint8_t* header = rom.data() + rom.size() - 10;
    if (header[8] == 0xE1 && header[9] == 0x8D && rom.size() < 0x100000) header[8] ^= 0xFF;
    try {
      Load(rom.data(), rom.size());
    } catch (...) {
      error = "the emulator can't load this file";
      return false;
    }
    loaded_ = true;
    rate_ = rate;
    WSwan_SetSoundRate(rate);
    buf_.resize(8192 * 2);

    const double t0 = cpu_seconds();
    scan();
    if (songs_.empty()) {
      // Some rips never switch on the timer that their driver runs on (port 0xA2):
      // the first WSR player, in_wsr, ran it regardless. Do as in_wsr did, then.
      timer_on_ = true;
      scan();
    }
    if (songs_.empty()) { error = "no song number of this file makes a sound"; return false; }
    // The footer names the song to start with: in many rips the numbers below it are
    // the game's sound effects. The tracks start there, and go round.
    const auto first = std::lower_bound(songs_.begin(), songs_.end(), static_cast<int>(footer[5]));
    if (first != songs_.end()) std::rotate(songs_.begin(), first, songs_.end());
    if (track < 0 || track >= static_cast<int>(songs_.size())) { error = "no such track"; return false; }
    song_ = songs_[track];

    const double t1 = cpu_seconds();
    look();
    if (debug_on())
      std::fprintf(stderr, "wsr: CPU time: %.0f ms to find the songs (all processes together), %.0f ms to find the end\n",
                   (t1 - t0) * 1000, (cpu_seconds() - t1) * 1000);
    start(song_);
    info.tracks = static_cast<int>(songs_.size());
    info.type = "WonderSwan WSR";
    char text[160];
    std::snprintf(text, sizeof(text), "song number %d", song_);
    info.comment = text;
    if (timer_on_) info.comment += ", timer switched on for it";
    if (found_.kind == Found::kLoop) {
      std::snprintf(text, sizeof(text), ", a loop of %.1f s from %.1f s on, played once", found_.loop_length,
                    found_.loop_start);
      info.comment += text;
    } else if (found_.kind == Found::kNothing) {
      std::snprintf(text, sizeof(text), ", end not found, stopped after %d s", kDefaultSeconds);
      info.comment += text;
    }
    info.duration_ms = static_cast<long>(frames_to_play_ * 1000 / rate_);
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t n = 0;
    while (n < frames && played_ < frames_to_play_) {
      if (have_ == 0) {
        have_ = frame(buf_.data());
        at_ = 0;
        if (!have_) break;
      }
      size_t take = std::min(frames - n, have_);
      take = static_cast<size_t>(std::min<uint64_t>(take, frames_to_play_ - played_));
      for (size_t i = 0; i < take; ++i) {
        int l = buf_[(at_ + i) * 2], r = buf_[(at_ + i) * 2 + 1];
        const uint64_t p = played_ + i;
        if (p >= fade_from_) {
          const int64_t left = static_cast<int64_t>(frames_to_play_ - p);
          const int64_t all = static_cast<int64_t>(frames_to_play_ - fade_from_);
          l = static_cast<int>(l * left / all);
          r = static_cast<int>(r * left / all);
        }
        out[(n + i) * 2] = static_cast<int16_t>(l);
        out[(n + i) * 2 + 1] = static_cast<int16_t>(r);
      }
      n += take;
      at_ += take;
      have_ -= take;
      played_ += take;
    }
    return n;
  }

 private:
  void start(int song) {
    Reset(static_cast<uint8>(song));
    g_loop = kNoLoop;
    if (timer_on_) WSwan_writeport(0xA2, 0x03);
    v30mz_timestamp = 0;
    have_ = at_ = 0;
    played_ = 0;
  }

  // one frame of the display, 1/75 s: returns the number of sample frames made
  size_t frame(int16_t* out) {
    while (!wsExecuteLine()) {}
    if (states_) snapshot();
    const int32 n = WSwan_SoundFlush(out, 8192);
    v30mz_timestamp = 0;
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

  void snapshot() {
    State s;
    const uint8 control = WSwan_SoundRead(0x90);
    const uint32 wave = static_cast<uint32>(WSwan_SoundRead(0x8F)) << 6;
    for (int ch = 0; ch < 4; ++ch) {
      uint64_t x = 1469598103934665603ull;
      auto mix = [&x](uint64_t v) { x = (x ^ v) * 1099511628211ull; };
      const bool on = control & (1 << ch);
      const bool voice = ch == 1 && (control & 0x20);  // plays samples: its volume is the sample
      const bool noise = ch == 3 && (control & 0x80);
      const uint8 volume = !on ? 0 : voice ? WSwan_SoundRead(0x94) : WSwan_SoundRead(0x88 + ch);
      if (volume) {
        mix(voice);
        mix(noise);
        if (!voice) {
          mix(WSwan_SoundRead(0x80 + ch * 2) | (WSwan_SoundRead(0x81 + ch * 2) << 8));
          if (noise) mix(WSwan_SoundRead(0x8E));
          else for (int i = 0; i < 16; ++i) mix(wsRAM[(wave + ch * 16 + i) & 0xFFFF]);
          if (ch == 2 && (control & 0x40)) { mix(WSwan_SoundRead(0x8C)); mix(WSwan_SoundRead(0x8D)); }
        }
        if (!x) x = 1;
      } else {
        x = 0;
      }
      s.note[ch] = x;
      s.volume[ch] = volume;
    }
    states_->push_back(s);
  }

  static double cpu_seconds() {
    double sum = 0;
    for (int who : {RUSAGE_SELF, RUSAGE_CHILDREN}) {
      struct rusage u;
      if (!getrusage(who, &u))
        sum += u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
    }
    return sum;
  }

  static bool loud(const int16_t* buf, size_t frames) {
    for (size_t i = 0; i < frames * 2; ++i)
      if (buf[i] > kQuiet || buf[i] < -kQuiet) return true;
    return false;
  }

  struct Heard {
    uint8_t sound;
    uint64_t what;
  };

  // Does the song number make a sound? And a number for what it does, to tell
  // whether two song numbers are the same song.
  Heard listen(int song) {
    std::vector<State> states;
    start(song);
    states_ = &states;
    Heard h = {0, 0};
    uint64_t made = 0;
    uint64_t want = static_cast<uint64_t>(scan_seconds_) * rate_;
    while (made < want) {
      const size_t n = frame(buf_.data());
      if (!n) break;
      made += n;
      if (!h.sound && loud(buf_.data(), n)) {
        h.sound = 1;
        want = std::min<uint64_t>(want, made + static_cast<uint64_t>(kScanMoreSeconds * rate_));
      }
    }
    states_ = nullptr;
    uint64_t x = 1469598103934665603ull;
    for (const State& s : states)
      for (int c = 0; c < 4; ++c) x = ((x ^ s.note[c]) * 1099511628211ull ^ s.volume[c]) * 1099511628211ull;
    h.what = x ^ states.size();
    return h;
  }

  // Which song numbers are there? Several processes share the work.
  void scan() {
    const int kWorkers = std::max(1, std::min(16, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN))));
    struct Message { int song; Heard heard; };
    std::vector<Heard> all(256, Heard{0, 0});
    std::vector<bool> done(256, false);
    std::vector<int> fds(kWorkers, -1);
    std::vector<pid_t> pids(kWorkers, -1);
    std::fflush(stdout);
    std::fflush(stderr);
    for (int w = 0; kWorkers > 1 && w < kWorkers; ++w) {
      int p[2];
      if (pipe(p)) break;
      const pid_t pid = fork();
      if (pid < 0) { close(p[0]); close(p[1]); break; }
      if (pid == 0) {
        close(p[0]);
        for (int k = 0; k < w; ++k) if (fds[k] >= 0) close(fds[k]);
        for (int s = w; s < 256; s += kWorkers) {
          Message m;
          std::memset(&m, 0, sizeof(m));
          m.song = s;
          m.heard = listen(s);
          if (write(p[1], &m, sizeof(m)) != static_cast<ssize_t>(sizeof(m))) break;
        }
        _exit(0);
      }
      close(p[1]);
      fds[w] = p[0];
      pids[w] = pid;
    }
    for (int w = 0; w < kWorkers; ++w) {
      if (fds[w] < 0) continue;
      for (;;) {
        Message m;
        size_t got = 0;
        while (got < sizeof(m)) {
          const ssize_t n = read(fds[w], reinterpret_cast<char*>(&m) + got, sizeof(m) - got);
          if (n <= 0) break;
          got += static_cast<size_t>(n);
        }
        if (got != sizeof(m)) break;
        if (m.song >= 0 && m.song < 256) { all[m.song] = m.heard; done[m.song] = true; }
      }
      close(fds[w]);
      int status;
      waitpid(pids[w], &status, 0);
    }
    for (int s = 0; s < 256; ++s)  // what no worker has done
      if (!done[s]) all[s] = listen(s);
    std::vector<uint64_t> seen;
    int same = 0;
    for (int s = 0; s < 256; ++s) {
      if (!all[s].sound) continue;
      if (std::find(seen.begin(), seen.end(), all[s].what) != seen.end()) { ++same; continue; }
      seen.push_back(all[s].what);
      songs_.push_back(s);
    }
    if (debug_on()) {
      std::fprintf(stderr, "wsr: %zu song numbers, %d more that do the same as an earlier one:", songs_.size(), same);
      for (int s : songs_) std::fprintf(stderr, " %d", s);
      std::fprintf(stderr, "\n");
    }
  }

  // Runs the song without playing it, to find out where it ends.
  void look() {
    std::vector<State> states;
    start(song_);
    states_ = &states;
    g_skipped = g_run = 0;
    uint64_t made = 0, last_sound = 0;
    bool any = false;
    double finding = 0;
    found_ = Found();
    const uint64_t limit = static_cast<uint64_t>(kLookSeconds) * rate_;
    uint64_t next_try = static_cast<uint64_t>(30) * rate_;
    const uint64_t silence = static_cast<uint64_t>(kSilenceSeconds * rate_);
    while (made < limit) {
      const size_t n = frame(buf_.data());
      if (!n) break;
      for (size_t i = 0; i < n; ++i)
        if (loud(buf_.data() + i * 2, 1)) { last_sound = made + i + 1; any = true; }
      made += n;
      if (any && made - last_sound >= silence) {
        found_.kind = Found::kSilence;
        found_.end = static_cast<double>(last_sound) / rate_ + 0.25;
        break;
      }
      if (!any && made >= 2 * silence) {  // it made a sound when the song numbers were tried
        found_.kind = Found::kSilence;
        found_.end = 1;
        break;
      }
      if (made >= next_try) {
        next_try += std::max<uint64_t>(next_try / 8, 5 * rate_);
        const double t = cpu_seconds();
        const bool is = LoopFinder(states).find(found_);
        finding += cpu_seconds() - t;
        if (is) break;
      }
    }
    if (debug_on()) std::fprintf(stderr, "wsr: %.0f ms of CPU time went into looking for a loop\n", finding * 1000);
    if (found_.kind == Found::kNothing && !LoopFinder(states).find(found_)) {
      // the chip is set to the same for a long time, but it sounds: a note left hanging
      size_t same = 0;
      while (same + 1 < states.size() &&
             !std::memcmp(&states[states.size() - 1], &states[states.size() - 2 - same], sizeof(State)))
        ++same;
      if (same > kSteadySeconds * kFramesPerSecond) {
        found_.kind = Found::kStill;
        found_.end = (states.size() - same) / kFramesPerSecond + 1;
      }
    }
    states_ = nullptr;
    if (const char* path = std::getenv("BBMP_CONSOLE_STATES")) {  // for looking into it
      if (FILE* f = std::fopen(path, "wb")) {
        for (const State& s : states)
          for (int c = 0; c < 4; ++c) {
            const uint64_t v = s.volume[c];
            std::fwrite(&s.note[c], 8, 1, f);
            std::fwrite(&v, 8, 1, f);
          }
        std::fclose(f);
      }
    }

    double fade = 0, length;
    switch (found_.kind) {
      case Found::kSilence: length = found_.end; break;
      case Found::kLoop:
      case Found::kStill: length = found_.end + kFadeSeconds; fade = kFadeSeconds; break;
      default: length = kDefaultSeconds; fade = kFadeSeconds; break;
    }
    frames_to_play_ = static_cast<uint64_t>(length * rate_);
    fade_from_ = frames_to_play_ - static_cast<uint64_t>(fade * rate_);
    if (debug_on()) {
      static const char* const names[] = {"nothing", "silence", "loop", "still"};
      std::fprintf(stderr,
                   "wsr: song %d: %s, end %.2f s, loop from %.2f s of %.2f s, looked at %.1f s, plays %.2f s, "
                   "%.1f%% of the CPU's time skipped\n",
                   song_, names[found_.kind], found_.end, found_.loop_start, found_.loop_length,
                   static_cast<double>(made) / rate_, length, 100.0 * g_skipped / (g_run + 1));
    }
  }

  bool loaded_ = false, timer_on_ = false;
  int rate_ = 48000, song_ = 0, scan_seconds_ = kScanSeconds;
  std::vector<int> songs_;
  std::vector<int16_t> buf_;
  size_t have_ = 0, at_ = 0;
  uint64_t played_ = 0, frames_to_play_ = 0, fade_from_ = 0;
  Found found_;
  std::vector<State>* states_ = nullptr;
};

}  // namespace

Engine* make_wsr_engine() { return new WsrEngine; }
