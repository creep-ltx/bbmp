// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-ay's engine "strc": Sound Tracker 1.1 songs compiled and packed into an AY file
// of type STRC (the "ZXAYSTRC" files of the Amiga's AY players).
//
// Written for this player in a clean room, from a description of the format only. The
// sound is made by the shared AY chip (Ayumi, by Peter Sovietov, MIT) at 50 ticks a
// second and 1.75 MHz.
//
// The song in such a file is a piece of the Spectrum's memory: a header with tables of
// addresses (samples, ornaments, volumes, mixer bytes, loops), the list of positions and
// the patterns, all found through addresses in that memory. The engine loads the piece
// into 64 KB of zeroed memory at the address the file names and reads everything from
// there, so addresses that point outside the piece read zeros, as on the Spectrum.
//
// The song plays its positions once (plus the fade the file asks for), at most one hour.
// Before it plays, it is run once without sound to see that it makes any; a silent song
// is refused.
//
// Test mode: with BBMP_STRC_LOG=<file>, the registers of every tick are written to that
// file, one line per tick ("<tick> <14 hex values> S|-", S when the envelope shape was
// written), after a first line "length <ticks> first-heard <tick or -1>".

#include "aychip.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ay {
namespace {

// Sound Tracker's tone periods for the AY at 1.75 MHz
const uint16_t kNotes[96] = {
  3832, 3600, 3424, 3200, 3032, 2856, 2696, 2544, 2400, 2272, 2136, 2016,
  1916, 1800, 1712, 1600, 1516, 1428, 1348, 1272, 1200, 1136, 1068, 1008,
   958,  900,  856,  800,  758,  714,  674,  636,  600,  568,  534,  504,
   479,  450,  428,  400,  379,  357,  337,  318,  300,  284,  267,  252,
   239,  225,  214,  200,  189,  178,  168,  159,  150,  142,  133,  126,
   119,  112,  107,  100,   94,   89,   84,   79,   75,   71,   66,   63,
    59,   56,   53,   50,   47,   44,   42,   39,   37,   35,   33,   31,
    29,   28,   26,   25,   23,   22,   21,   19,   18,   17,   16,   15};

const long kLongest = 180000;      // ticks: one hour at 50 a second
const int kMostPositions = 4096;

// where the tables sit in the song, from its first byte
enum : uint16_t {
  kDelay = 0, kRows = 1, kTones = 2, kOrnaments = 34, kVolumes = 66, kMixers = 98,
  kLoops = 130, kPositions = 162
};

// The song's memory and its address arithmetic (everything wraps at 64 KB).
struct Memory {
  std::vector<uint8_t> m = std::vector<uint8_t>(65536, 0);
  uint8_t at(unsigned a) const { return m[a & 0xFFFF]; }
  uint16_t word(unsigned a) const { return at(a) | (at(a + 1) << 8); }
};

struct Channel {
  uint16_t pattern = 0;
  uint8_t note = 0, ornament = 0, sample = 0, remembered = 0;
  uint8_t step = 0, left = 0, loop_start = 0, loop_length = 0;
  uint8_t volume = 0;
};

// One player of a song: the state of the tracker and the registers it makes.
class Player {
 public:
  Player(const Memory& mem, uint16_t base) : mem_(mem), base_(base), position_(base + kPositions) {}

  // Moves on by one tick and fills `regs`.
  void tick(Regs& regs) {
    bool new_row = false;
    if (--tick_count_ == 0) {
      new_row = true;
      tick_count_ = mem_.at(base_ + kDelay);
      if (--row_count_ == 0) {
        row_count_ = mem_.at(base_ + kRows);
        if (mem_.at(position_ + 1) == 0) position_ = base_ + kPositions;
        for (int c = 0; c < 3; ++c) ch_[c].pattern = mem_.word(position_ + 2 * c);
        transpose_ = mem_.at(position_ + 6);
        position_ = static_cast<uint16_t>(position_ + 7);
      }
    }
    uint8_t mixer = 0xFF;
    uint16_t period[3];
    for (int c = 0; c < 3; ++c) {
      Channel& h = ch_[c];
      if (new_row) row(h);
      else step(h);
      const uint8_t s = h.sample & 15, o = h.ornament & 15, p = h.step;
      const uint16_t offset = mem_.word(mem_.word(base_ + kTones + 2 * s) + 2 * p);
      const uint8_t shift = mem_.at(mem_.word(base_ + kOrnaments + 2 * o) + p);
      unsigned index = static_cast<uint8_t>(transpose_ + shift + h.note);
      if (index > 95) index = 95;
      period[c] = static_cast<uint16_t>((kNotes[index] + offset) & 0xFFF);
      const uint8_t v = mem_.at(mem_.word(base_ + kVolumes + 2 * s) + p);
      if (v == 0) continue;
      const uint8_t m = mem_.at(mem_.word(base_ + kMixers + 2 * s) + p);
      if (!(m & 0x40)) mixer &= ~(1 << c);
      if (!(m & 0x80)) {
        mixer &= ~(8 << c);
        noise_ = m & 31;
      }
      h.volume = (h.volume & 16) | v;
    }
    for (int c = 0; c < 3; ++c) {
      regs.r[2 * c] = period[c] & 0xFF;
      regs.r[2 * c + 1] = period[c] >> 8;
      regs.r[8 + c] = ch_[c].volume;
    }
    regs.r[6] = noise_;
    regs.r[7] = mixer;
    regs.r[11] = envelope_;
    regs.r[12] = 0;
    regs.r[13] = 0;
    regs.shape_written = false;
    if (shape_) {
      regs.r[13] = shape_;
      regs.shape_written = true;
      shape_ = 0;
    }
  }

 private:
  // The channel's next entry in its pattern.
  void row(Channel& h) {
    const uint8_t b = mem_.at(h.pattern);
    if (b == 0xFF) {                       // a rest
      h.pattern++;
      h.remembered = h.sample = 0;
      h.volume = 0;
      step(h);
      return;
    }
    if (b >= 0x80) {                       // the sound goes on
      h.pattern++;
      step(h);
      return;
    }
    const uint8_t what = mem_.at(h.pattern + 1), value = mem_.at(h.pattern + 2);
    h.pattern = static_cast<uint16_t>(h.pattern + 3);
    h.note = b;
    if (what >> 4) h.remembered = what >> 4;
    h.sample = h.remembered;
    h.step = 0;
    h.left = 32;
    h.loop_start = mem_.at(base_ + kLoops + 2 * h.sample);
    h.loop_length = mem_.at(base_ + kLoops + 2 * h.sample + 1);
    const int command = what & 15;
    if (command == 1) {
      h.ornament = 0;
      h.volume = 0;
    } else if (command == 15) {
      h.ornament = value & 15;
      h.volume = 0;
    } else if (command >= 7) {
      h.ornament = 0;
      h.volume = 16;
      shape_ = static_cast<uint8_t>(command);
      envelope_ = value;
    }
  }

  // One step on in the sample, looping or ending it.
  static void step(Channel& h) {
    for (int round = 0; round < 2; ++round) {
      h.step = (h.step + 1) & 31;
      h.left--;
      if (h.left) return;
      if (h.loop_start) {
        h.step = static_cast<uint8_t>(h.loop_start - 1);
        h.left = h.loop_length;
        return;
      }
      h.sample = 0;
      h.volume = 0;
    }
  }

  const Memory& mem_;
  uint16_t base_;
  uint16_t position_;
  uint8_t tick_count_ = 1, row_count_ = 1;
  uint8_t transpose_ = 0, envelope_ = 0, noise_ = 0, shape_ = 0;
  std::array<Channel, 3> ch_{};
};

class StrcEngine : public FrameEngine {
 public:
  ~StrcEngine() override {
    if (log_) std::fclose(log_);
  }

  void open(const std::string&, const std::vector<uint8_t>& d, int track, int rate,
            Info& info) override {
    const long L = static_cast<long>(d.size());
    if (L < 20 || std::memcmp(d.data(), "ZXAYSTRC", 8) != 0) throw Refused("not an AY file of type STRC");
    const int songs = d[16] + 1;
    if (track >= songs) throw Refused("no such track");
    auto sword = [&](long p) { return static_cast<long>(static_cast<int16_t>((d[p] << 8) | d[p + 1])); };
    auto pointer = [&](long p) {
      if (p < 0 || p + 1 >= L) throw Refused("the AY file is broken");
      const long w = sword(p);
      if (w == 0 || p + w < 0 || p + w >= L) throw Refused("the AY file is broken");
      return p + w;
    };
    const long list = pointer(18);
    const long entry = list + 4L * track;
    const long name = pointer(entry);
    const long S = pointer(entry + 2);
    if (S + 174 > L) throw Refused("the song is broken");

    const unsigned base = (d[S + 4] << 8) | d[S + 5];
    const long fade = sword(S + 6);
    long n = L - (S + 10);
    if (n > 65536L - base) n = 65536L - base;
    std::memcpy(mem_.m.data() + base, d.data() + S + 10, static_cast<size_t>(n));
    base_ = static_cast<uint16_t>(base);

    const uint8_t delay = mem_.at(base + kDelay), rows = mem_.at(base + kRows);
    if (delay == 0 || rows == 0) throw Refused("the song has no speed");
    long positions = 0;
    for (unsigned at = base + kPositions; mem_.at(at + 1) != 0; at += 7) {
      if (++positions > kMostPositions) throw Refused("the list of positions has no end");
    }
    if (positions == 0) throw Refused("the song has no positions");
    length_ = positions * rows * delay + (fade > 0 ? fade : 0);
    if (length_ > kLongest) length_ = kLongest;

    if (const char* to = std::getenv("BBMP_STRC_LOG")) log_ = std::fopen(to, "w");
    long heard = -1;
    {
      Player quiet(mem_, base_);
      for (long t = 0; t < length_; ++t) {
        Regs r;
        quiet.tick(r);
        if (r.r[8] || r.r[9] || r.r[10]) {
          heard = t + 1;
          break;
        }
      }
    }
    if (log_) std::fprintf(log_, "length %ld first-heard %ld\n", length_, heard);
    if (heard < 0) throw Refused("the song is silent");

    auto text = [&](long t) {
      std::string s;
      if (t < 0) return s;
      for (long i = t; i < L && i < t + 256 && d[i]; ++i) s += static_cast<char>(d[i]);
      return s;
    };
    info.tracks = songs;
    info.title = text(name);
    if (const long a = sword(12)) info.author = text(12 + a);
    if (const long c = sword(14)) info.comment = text(14 + c);
    info.type = "Sound Tracker (Raxoft compiler)";
    info.duration_ms = length_ * 20;

    player_.reset(new Player(mem_, base_));
    setup(rate, 50.0, 1750000.0, Chip::AY, Layout::ABC);
  }

 protected:
  bool tick() override {
    if (played_ >= length_) return false;
    Regs r;
    player_->tick(r);
    if (log_) {
      std::fprintf(log_, "%ld", played_);
      for (int i = 0; i < 14; ++i) std::fprintf(log_, " %02X", r.r[i]);
      std::fprintf(log_, " %c\n", r.shape_written ? 'S' : '-');
    }
    psg().write(0, r);
    ++played_;
    return true;
  }

 private:
  Memory mem_;
  uint16_t base_ = 0;
  long length_ = 0, played_ = 0;
  std::unique_ptr<Player> player_;
  FILE* log_ = nullptr;
};

}  // namespace

std::unique_ptr<Engine> make_strc() { return std::make_unique<StrcEngine>(); }

}  // namespace ay
