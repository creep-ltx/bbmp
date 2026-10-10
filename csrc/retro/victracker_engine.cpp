// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro, engine "victracker": VIC-TRACKER (Commodore VIC-20, Daniel Kahlin,
// 1994-2004), *.vt.
//
// A .vt file is what the tracker saves: its song memory from $3300 on (a PRG with the
// load address), cut after the last pattern that is used. The tracker's own player
// (musiced/player.asm of VIC-TRACKER 2.0, BSD licence) is assembled at build time with
// dasm into vt_player.h; here it runs on a 6502 with the song where the tracker keeps
// it, and what it writes to the VIC's sound registers ($900A-$900E) is heard through
// a VIC sound generator written for this engine.
//
// The tracker calls the player from a timer interrupt; how often depends on the song's
// play mode (PAL, NTSC, 2x to 4x a frame; musiced/playersupport.asm). A song made on a
// PAL machine gets the PAL mode when it is created, one made on an NTSC machine the
// NTSC mode, so the mode also tells the machine, whose clock sets the pitch.
//
// The VIC's sound, as the chip does it (described by the VICE team's research): each
// of the three tone voices has a 7-bit counter clocked at the CPU clock divided by 16,
// 8 and 4. When it runs out it is reloaded from the voice's register and shifts an
// 8-bit register that feeds back its inverted top bit, while the voice is on, and
// zeroes when it is off; the low bit is the output. That makes a square wave of 16
// shifts. The noise voice is the same at a divider of 2, with a 16-bit LFSR whose low
// bit's rising edges clock its shift register. The four outputs are added and scaled by
// the volume; the machine's output stage is a low-pass at 1.6 kHz (the RC values of the
// VIC-20's audio circuit) and a high-pass, here at 50 Hz: the circuit's 159 Hz took more
// bass away than the recordings on the tracker's home page have (measured on five songs;
// from 125 Hz up the levels per octave agree with them within 1 dB).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "cybertracker_6502.h"
#include "engine.h"
#include "vt_player.h"   // generated: vt_player[] at VT_PLAYER_ORG, and the player's labels

namespace {

using namespace retro;

const uint16_t kSongAt = 0x3300;       // the tracker's song memory (TuneStart)
const size_t kSongMost = 0x2a48;       // TuneEnd - TuneStart: the header and 128 patterns

struct Clock {
  double cpu;          // Hz
  double frame;        // cycles between two timer interrupts of "one a frame"
};
const Clock kPal = {1108405.0, 22150.0};      // include/vic20.i: SCREENTIME_PAL
const Clock kNtsc = {1022727.0, 16963.0};     // SCREENTIME_NTSC

class Vic;

class Machine {
 public:
  uint8_t ram[65536];
  Cpu6502<Machine> cpu;
  Vic* vic = nullptr;

  Machine() : cpu(*this) { std::memset(ram, 0, sizeof(ram)); }
  // a copy with a processor of its own (the processor holds on to its memory)
  Machine(const Machine& o) : cpu(*this) {
    std::memcpy(ram, o.ram, sizeof(ram));
    cpu.a = o.cpu.a; cpu.x = o.cpu.x; cpu.y = o.cpu.y; cpu.sp = o.cpu.sp; cpu.pc = o.cpu.pc;
    cpu.set_flags(o.cpu.flags());
    cpu.cycles = o.cpu.cycles;
    vic = o.vic;
  }
  Machine& operator=(const Machine&) = delete;
  uint8_t read(uint16_t a) { return ram[a]; }
  void write(uint16_t a, uint8_t v);
};

// The VIC's four voices and its volume.
class Vic {
 public:
  explicit Vic(double clock, int rate) : clock_(clock), rate_(rate) {
    cyclesPerSample_ = clock / rate;
    const double dt = 1.0 / rate;
    lowBeta_ = dt / (dt + 1e-4);     // 1 kOhm, 100 nF
    highBeta_ = dt / (dt + 3.2e-3);  // 50 Hz: see the top of the file
    for (int j = 0; j < 4; j++) {
      counter_[j] = 1;
      shift_[j] = 0;
      reg_[j] = 0;
    }
  }

  void store(unsigned reg, uint8_t value) {
    if (reg >= 0x0a && reg <= 0x0d) reg_[reg - 0x0a] = value;
    else if (reg == 0x0e) volume_ = value & 0x0f;
  }

  // Runs the chip for <cycles> clock cycles and adds the samples that are due to <out>.
  void run(uint64_t cycles, std::deque<int16_t>& out) {
    while (cycles--) {
      for (int j = 0; j < 4; j++) {
        if (--counter_[j] > 0) continue;
        unsigned period = (~reg_[j]) & 127;
        if (!period) period = 128;
        counter_[j] = period << kDivider[j];
        const bool on = reg_[j] & 0x80;
        if (j < 3) {
          shift_[j] = static_cast<uint8_t>(shift_[j] << 1 | (((shift_[j] >> 7) ^ 1) & on));
        } else {
          const bool edge = (lfsr_ & 1) && !lfsrLow_;
          if (edge) shift_[3] = static_cast<uint8_t>(shift_[3] << 1 | (((shift_[3] >> 7) ^ 1) & on));
          const unsigned b = ((lfsr_ >> 3) ^ (lfsr_ >> 12) ^ (lfsr_ >> 14) ^ (lfsr_ >> 15) ^ 1) & 1;
          lfsrLow_ = lfsr_ & 1;
          lfsr_ = static_cast<uint16_t>(lfsr_ << 1 | ((b & on) ^ 1));
        }
      }
      const unsigned level = (shift_[0] & 1) + (shift_[1] & 1) + (shift_[2] & 1) +
                             (shift_[3] & 1 & (reg_[3] >> 7));
      sum_ += level * volume_;
      if (++inSample_ >= cyclesPerSample_ - carry_) {
        carry_ += inSample_ - cyclesPerSample_;
        // 0..60 as the average level of the sample
        const double level = sum_ / inSample_;
        sum_ = 0;
        inSample_ = 0;
        low_ += lowBeta_ * (level - low_);
        high_ += highBeta_ * (low_ - high_);
        double s = (low_ - high_) * (32767.0 / 60.0);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out.push_back(static_cast<int16_t>(std::lrint(s)));
      }
    }
  }

 private:
  static constexpr int kDivider[4] = {4, 3, 2, 1};
  double clock_;
  int rate_;
  double cyclesPerSample_;
  int counter_[4];
  uint8_t shift_[4];
  uint8_t reg_[4];
  uint8_t volume_ = 0;
  uint16_t lfsr_ = 0;
  bool lfsrLow_ = false;
  double sum_ = 0;
  unsigned inSample_ = 0;
  double carry_ = 0;
  double lowBeta_, highBeta_, low_ = 0, high_ = 0;
};

void Machine::write(uint16_t a, uint8_t v) {
  if (a >= 0x9000 && a < 0x9010) {
    if (vic) vic->store(a & 15, v);
    return;
  }
  ram[a] = v;
}

class VictrackerEngine : public Engine {
 public:
  bool open(const std::vector<uint8_t>& file, const std::string&, int track, int rate,
            SongInfo& info, std::string& error) override {
    if (file.size() < 2 + 0x3c8 || le16(file.data()) != kSongAt || file[2] != 'T' ||
        (file[3] != '1' && file[3] != '0')) {
      error = "not a VIC-TRACKER song";
      return false;
    }
    if (file[3] == '0') {
      // VIC-TRACKER 1.0's songs are converted by the tracker when it loads them; none
      // are known to be about, so that is not done here
      error = "a VIC-TRACKER 1.0 song (format T0), which is not supported";
      return false;
    }
    if (file.size() - 2 > kSongMost) {
      error = "the song is larger than the tracker's song memory";
      return false;
    }
    const uint8_t* song = file.data() + 2;
    const int songs = song[7] ? song[7] : 1;
    info.tracks = songs > 14 ? 14 : songs;
    if (track < 0 || track >= info.tracks) {
      error = "no such track";
      return false;
    }
    info.title = field(song + kTitleAt, 16);
    info.author = field(song + kAuthorAt, 16);
    const std::string year = field(song + kYearAt, 4);
    info.comment = year.empty() ? "" : year;
    info.type = std::string("VIC-TRACKER ") + char('0' + (song[2] % 10)) + "." + char('0' + (song[3] % 10));

    // the play mode: the machine and how often the player is called
    const unsigned mode = song[4];
    static const int kTimes[11] = {1, 1, 2, 3, 4, 1, 2, 3, 4, 1, 1};
    const bool ntsc = mode >= 5 && mode <= 8;
    const Clock& clock = ntsc ? kNtsc : kPal;
    double frame = clock.frame;
    // modes 9 and 10 follow an outside clock (Sync24 and Sync48, 24 or 48 pulses a beat;
    // the player is called every pulse or every second one): taken as 120 beats a
    // minute, that is 48 calls a second in both
    if (mode == 9 || mode == 10) frame = clock.cpu / 48.0;
    period_ = frame / (mode <= 10 ? kTimes[mode] : 1);
    clockHz_ = clock.cpu;

    machine_.reset(new Machine);
    Machine& m = *machine_;
    std::memcpy(m.ram + kSongAt, song, file.size() - 2);
    std::memcpy(m.ram + VT_PLAYER_ORG, vt_player, sizeof(vt_player));
    m.ram[VT_pl_ThisSong] = static_cast<uint8_t>(track);
    m.cpu.sp = 0xff;
    if (!m.cpu.call(VT_pl_Init, 1000000)) {
      error = "the player didn't start the song";
      return false;
    }

    // Once through without sound, for the length
    {
      Machine dry(m);
      dry.vic = nullptr;
      long long ticks = 0;
      const long long most = static_cast<long long>(3600.0 * clock.cpu / period_);
      bool ended = false;
      while (ticks < most) {
        ticks++;
        if (!tick(dry, ended)) break;
        if (ended) break;
      }
      endTicks_ = ended ? ticks : most;
      if (ended) info.duration_ms = static_cast<long long>(ticks * period_ * 1000.0 / clock.cpu);
      if (debug_on())
        std::fprintf(stderr, "victracker: mode %u, %.3f Hz, %s after %.1f s\n", mode,
                     clock.cpu / period_, ended ? "back at the start" : "no end found",
                     endTicks_ * period_ / clock.cpu);
    }

    vic_.reset(new Vic(clock.cpu, rate));
    m.vic = vic_.get();
    m.write(0x900a, 0);
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    Machine& m = *machine_;
    while (samples_.size() < frames && !done_) {
      if (ticks_ >= endTicks_) {
        // what still sounds after the last call dies away for a moment
        if (tail_ >= 0.25 * clockHz_) {
          done_ = true;
          break;
        }
        vic_->run(static_cast<uint64_t>(clockHz_ / 100), samples_);
        tail_ += clockHz_ / 100;
        continue;
      }
      bool ended = false;
      if (!tick(m, ended)) {
        done_ = true;
        break;
      }
      ticks_++;
      // the player's writes land at once; then the time until the next call passes
      position_ += period_;
      const uint64_t whole = static_cast<uint64_t>(position_);
      position_ -= whole;
      vic_->run(whole, samples_);
    }
    size_t n = 0;
    for (; n < frames && !samples_.empty(); n++) {
      out[2 * n] = out[2 * n + 1] = samples_.front();
      samples_.pop_front();
    }
    return n;
  }

 private:
  // offsets in the song (fileformat.txt of VIC-TRACKER 2.0)
  static constexpr unsigned kHeader = 8 + 4 * 14;             // up to pl_Arpeggios
  static constexpr unsigned kTitleAt = kHeader + 256 + 32 + 60;
  static constexpr unsigned kAuthorAt = kTitleAt + 16;
  static constexpr unsigned kYearAt = kAuthorAt + 16;

  // One call of the player. <ended>: it went from the song's last step back to its
  // first or its repeat step.
  static bool tick(Machine& m, bool& ended) {
    const uint8_t step = m.ram[VT_pl_Step];
    const uint8_t row = m.ram[VT_pl_PatternPos];
    const uint8_t end = m.ram[VT_pl_ThisEndStep];
    if (!m.cpu.call(VT_pl_Play, 1000000)) return false;
    if (step == end && (m.ram[VT_pl_Step] != step || m.ram[VT_pl_PatternPos] < row)) ended = true;
    return true;
  }

  std::unique_ptr<Machine> machine_;
  std::unique_ptr<Vic> vic_;
  std::deque<int16_t> samples_;
  double period_ = 22150;
  double clockHz_ = 1108405;
  double position_ = 0;
  double tail_ = 0;
  long long ticks_ = 0, endTicks_ = 0;
  bool done_ = false;
};

}  // namespace

REGISTER_ENGINE("victracker", VictrackerEngine)
