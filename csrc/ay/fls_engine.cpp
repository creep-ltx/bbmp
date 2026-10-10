// SPDX-License-Identifier: GPL-3.0-or-later
// Flash Tracker (.fls), a ZX Spectrum tracker on the lines of Sound Tracker.
//
// A port of the Flash Tracker player of Ay_Emul 3.x by Sergey Bulba (Players.pas:
// FLS_Get_Registers, and the search for the address the module was compiled for;
// "You can use this source code freely, only do references to author").
//
// A module is a piece of the Spectrum's memory with pointers that are addresses
// in that memory. Where in memory it was is not written in the file and has to
// be found from the module's structure.

#include "aychip.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace ay {
namespace {

// Sound Tracker's notes
const uint16_t kTable[96] = {
  0xef8, 0xe10, 0xd60, 0xc80, 0xbd8, 0xb28, 0xa88, 0x9f0, 0x960, 0x8e0, 0x858, 0x7e0,
  0x77c, 0x708, 0x6b0, 0x640, 0x5ec, 0x594, 0x544, 0x4f8, 0x4b0, 0x470, 0x42c, 0x3f0,
  0x3be, 0x384, 0x358, 0x320, 0x2f6, 0x2ca, 0x2a2, 0x27c, 0x258, 0x238, 0x216, 0x1f8,
  0x1df, 0x1c2, 0x1ac, 0x190, 0x17b, 0x165, 0x151, 0x13e, 0x12c, 0x11c, 0x10b, 0xfc,
  0xef, 0xe1, 0xd6, 0xc8, 0xbd, 0xb2, 0xa8, 0x9f, 0x96, 0x8e, 0x85, 0x7e,
  0x77, 0x70, 0x6b, 0x64, 0x5e, 0x59, 0x54, 0x4f, 0x4b, 0x47, 0x42, 0x3f,
  0x3b, 0x38, 0x35, 0x32, 0x2f, 0x2c, 0x2a, 0x27, 0x25, 0x23, 0x21, 0x1f,
  0x1d, 0x1c, 0x1a, 0x19, 0x17, 0x16, 0x15, 0x13, 0x12, 0x11, 0x10, 0xf};

const int kSteps = 4096;        // commands for one row before the song counts as broken

struct Broken {};

struct Channel {
  uint16_t address = 0;         // in the pattern
  uint16_t ornament = 0, sample = 0;
  uint16_t tone = 0;
  uint8_t sample_length = 0, sample_loop = 0, sample_at = 0;
  uint8_t amplitude = 0;
  uint8_t skip = 0;             // rows between two notes
  uint8_t note = 0;
  int8_t skip_counter = 0;
  int8_t sample_ticks = -1;
  bool envelope = false, with_ornament = false;
};

class Player {
 public:
  void start(const uint8_t* memory, uint16_t base) {
    mem_ = memory;
    base_ = base;
    positions_ = word(base);
    ornaments_ = word(base + 2);
    samples_ = word(base + 4);
    delay_ = at(positions_);
    delay_counter_ = 1;
    position_ = 0;
    looped_ = false;
    for (Channel& c : ch_) c = Channel();
    std::memset(regs_.r, 0, sizeof(regs_.r));
    set_patterns();
  }

  bool looped() const { return looped_; }

  void tick(Regs& out) {
    regs_.shape_written = false;
    if (--delay_counter_ == 0) {
      if (--ch_[0].skip_counter < 0) {
        if (at(ch_[0].address) == 255) {
          ++position_;
          if (at(positions_ + 1 + position_) == 0) {
            position_ = 0;
            looped_ = true;
          }
          set_patterns();
        }
        interpret(ch_[0]);
      }
      for (int i = 1; i < 3; ++i)
        if (--ch_[i].skip_counter < 0) interpret(ch_[i]);
      delay_counter_ = delay_;
    }
    mixer_ = 0;
    for (Channel& c : ch_) registers(c);
    regs_.r[7] = mixer_;
    for (int i = 0; i < 3; ++i) {
      regs_.r[i * 2] = ch_[i].tone & 0xFF;
      regs_.r[i * 2 + 1] = (ch_[i].tone >> 8) & 0x0F;
      regs_.r[8 + i] = ch_[i].amplitude;
    }
    out = regs_;
  }

 private:
  uint8_t at(unsigned a) const { return mem_[a & 0xFFFF]; }
  uint16_t word(unsigned a) const { return static_cast<uint16_t>(at(a) | (at(a + 1) << 8)); }

  void set_patterns() {
    const uint8_t pattern = at(positions_ + 1 + position_);
    // the pointers of pattern 1 come after the module's three pointers
    const unsigned pointers = base_ + 6 + (static_cast<unsigned>(pattern) - 1) * 6;
    for (int i = 0; i < 3; ++i) ch_[i].address = word(pointers + i * 2);
  }

  void interpret(Channel& c) {
    bool quit = false;
    for (int steps = 0; !quit; ++steps) {
      if (steps > kSteps) throw Broken();
      const uint8_t b = at(c.address);
      if (b <= 0x5F) {
        c.note = b;
        c.sample_at = 0;
        c.sample_ticks = 0x20;
        quit = true;
      } else if (b <= 0x6F) {
        const unsigned s = samples_ + (b - 0x60) * 4;
        c.sample_loop = at(s);
        c.sample_length = at(s + 1);
        c.sample = word(s + 2);
      } else if (b == 0x70) {
        c.with_ornament = false;
        c.envelope = false;
      } else if (b <= 0x7F) {
        c.ornament = word(ornaments_ + (b - 0x71) * 2);
        c.with_ornament = true;
        c.envelope = false;
      } else if (b == 0x80) {
        c.sample_ticks = -1;
        quit = true;
      } else if (b == 0x81) {
        quit = true;
      } else if (b <= 0x8E) {
        regs_.r[13] = b - 0x80;
        regs_.shape_written = true;
        c.envelope = true;
        c.with_ornament = false;
        ++c.address;
        regs_.r[11] = at(c.address);
      } else {
        c.skip = static_cast<uint8_t>(b - 0xA1);
      }
      ++c.address;
    }
    c.skip_counter = static_cast<int8_t>(c.skip);
  }

  void registers(Channel& c) {
    if (c.sample_ticks >= 0) {
      --c.sample_ticks;
      if (c.sample_ticks == 0) {
        if (c.sample_loop == 0) {
          --c.sample_ticks;
          c.amplitude = 0;
          mixer_ >>= 1;
          return;
        }
        c.sample_ticks = static_cast<int8_t>(c.sample_length);
        c.sample_at = static_cast<uint8_t>(c.sample_loop - 1);
      }
      const uint8_t b0 = at(c.sample + c.sample_at * 3);
      const uint8_t b1 = at(c.sample + c.sample_at * 3 + 1);
      c.amplitude = b0 & 15;
      if (c.envelope) c.amplitude |= 16;
      if (b1 & 0x80) mixer_ |= 64;
      else regs_.r[6] = b1 & 31;
      if (b1 & 64) mixer_ |= 8;
      uint8_t j = c.with_ornament ? at(c.ornament + c.sample_at) : 0;
      j = static_cast<uint8_t>(j + c.note);
      if (j > 0x5F) j = 0x5F;
      uint16_t tone = static_cast<uint16_t>(((static_cast<unsigned>(b0) << 4) & 0xF00) +
                                            at(c.sample + c.sample_at * 3 + 2));
      if ((b1 & 32) == 0) tone = static_cast<uint16_t>(-tone);
      c.tone = static_cast<uint16_t>(tone + kTable[j]) & 0xFFF;
      c.sample_at = (c.sample_at + 1) & 31;
    } else {
      c.amplitude = 0;
    }
    mixer_ >>= 1;
  }

  const uint8_t* mem_ = nullptr;
  unsigned base_ = 0;
  uint16_t positions_ = 0, ornaments_ = 0, samples_ = 0;
  uint8_t delay_ = 0, delay_counter_ = 1, position_ = 0;
  uint8_t mixer_ = 0;
  bool looped_ = false;
  Channel ch_[3];
  Regs regs_;
};

// The address the module was compiled for, or -1. As Ay_Emul looks for it: the
// last ornament is 32 bytes before the first sample, the byte before the first
// pattern is the zero that ends the list of positions, and the first pattern's
// second channel begins where its first channel ends.
long find_base(const std::vector<uint8_t>& d) {
  const long size = static_cast<long>(d.size());
  auto word = [&d](long a) { return static_cast<long>(d[a] | (d[a + 1] << 8)); };
  if (size < 64) return -1;
  const long ornaments = word(2), samples = word(4);
  const long pattern_a = word(6), pattern_b = word(8);
  for (long base = ornaments - 16; base >= 0; --base) {
    const long first_sample = samples + 2 - base;
    if (first_sample < 8 || first_sample + 1 >= size) continue;
    const long sample_body = word(first_sample) - base;
    if (sample_body < 8 || sample_body >= size) continue;
    const long ornament_body = word(first_sample - 4) - base;
    if (ornament_body < 6 || ornament_body >= size) continue;
    if (sample_body - ornament_body != 0x20) continue;
    const long b = pattern_b - base;
    if (b <= 21 || b >= size) continue;
    long a = pattern_a - base;
    if (a <= 20 || a >= size) continue;
    if (d[a - 1] != 0) continue;
    while (a < size && d[a] != 255) {
      do {
        const uint8_t v = d[a];
        if (v <= 0x5F || v == 0x80 || v == 0x81) {
          ++a;
          break;
        }
        if (v >= 0x82 && v <= 0x8E) ++a;
        ++a;
      } while (a < size);
    }
    if (a + 1 == b) return base;
  }
  return -1;
}

const double kFrameHz = 50.0;     // as ZXTune plays the Spectrum formats of this player (a Pentagon has 48.83)
const long kLongest = static_cast<long>(60 * 60 * 50);

class Fls : public FrameEngine {
 public:
  void open(const std::string&, const std::vector<uint8_t>& data, int track, int rate,
            Info& info) override {
    if (track != 0) throw Refused("no such track");
    if (data.size() < 64 || data.size() > 65536) throw Refused("not a Flash Tracker module");
    const long base = find_base(data);
    if (base < 0) throw Refused("not a Flash Tracker module: the address it was compiled for can't be found");
    const long size = static_cast<long>(data.size());
    if (base + size > 65536) throw Refused("not a Flash Tracker module");
    auto word = [&data](long a) { return static_cast<long>(data[a] | (data[a + 1] << 8)); };
    const long positions = word(0) - base, ornaments = word(2) - base, samples = word(4) - base;
    if (ornaments < 12 || samples < ornaments || positions <= samples || positions + 2 >= size ||
        ((samples - ornaments) & 1) || ((positions - samples) & 3) || (ornaments - 6) % 6 != 0)
      throw Refused("not a Flash Tracker module: its tables are not where they should be");
    const long patterns = (ornaments - 6) / 6;
    if (data[positions] == 0) throw Refused("the module has no speed");
    // every position names a pattern that is there, and the patterns are inside
    long count = 0;
    for (long p = positions + 1;; ++p, ++count) {
      if (p >= size) throw Refused("the list of positions has no end");
      if (data[p] == 0) break;
      if (data[p] > patterns) throw Refused("a position names a pattern that isn't there");
      for (int c = 0; c < 3; ++c) {
        const long a = word(6 + (data[p] - 1) * 6 + c * 2) - base;
        if (a < positions || a >= size) throw Refused("a pattern is outside the module");
      }
    }
    if (count == 0) throw Refused("the module has no positions");
    for (long s = samples; s < positions; s += 4) {
      const long a = word(s + 2) - base;
      if (a < 0 || a + 96 > size) throw Refused("a sample is outside the module");
    }
    for (long o = ornaments; o < samples; o += 2) {
      const long a = word(o) - base;
      if (a < 0 || a + 32 > size) throw Refused("an ornament is outside the module");
    }

    mem_.assign(65536, 0);
    std::memcpy(mem_.data() + base, data.data(), data.size());
    base_ = static_cast<uint16_t>(base);

    // once through without sound, for the length
    Player p;
    p.start(mem_.data(), base_);
    long ticks = 0;
    bool heard = false;
    try {
      while (ticks < kLongest) {
        Regs r;
        p.tick(r);
        if (p.looped()) break;
        heard = heard || (r.r[8] | r.r[9] | r.r[10]) != 0;
        ++ticks;
      }
    } catch (const Broken&) {
      throw Refused("the module's patterns are broken");
    }
    if (ticks >= kLongest) throw Refused("the module has no end");
    if (ticks < 1 || !heard) throw Refused("the module is silent");
    length_ = ticks;

    info.tracks = 1;
    info.type = "Flash Tracker";
    info.duration_ms = static_cast<long>(length_ * 1000.0 / kFrameHz);
    player_.start(mem_.data(), base_);
    setup(rate, kFrameHz, 1750000.0, Chip::AY, Layout::ABC);
  }

 protected:
  bool tick() override {
    if (played_ >= length_) return false;
    Regs r;
    try {
      player_.tick(r);
    } catch (const Broken&) {
      return false;
    }
    psg().write(0, r);
    ++played_;
    return true;
  }

 private:
  std::vector<uint8_t> mem_;
  uint16_t base_ = 0;
  Player player_;
  long length_ = 0, played_ = 0;
};

}  // namespace

std::unique_ptr<Engine> make_fls() {
  return std::make_unique<Fls>();
}

}  // namespace ay
