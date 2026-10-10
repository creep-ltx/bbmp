// SPDX-License-Identifier: GPL-3.0-or-later
// Fuxoft AY Language (.fxm) and its wrapping in an AY file (.amad, "ZXAYAMAD"):
// the music of Frantisek Fuka's ZX Spectrum 128 games, a small programming
// language with jumps, calls and loops, one program per channel.
//
// The interpreter is a port of FXM_Get_Registers of Ay_Emul 3.x by Sergey Bulba
// (Players.pas; "You can use this source code freely, only do references to
// author"), which also documents the format; the header of the AMAD wrapping is
// read as Ay_Emul reads it. It was then compared, register by register, with the
// original Z80 player running the same songs (from AY rips of the games), and
// changed where the two differed. Frantisek Fuka's own description of the language
// (github.com/fuxoft/fxmasm, MUSIC_FORMAT.md) was read beside it. Where the song
// ends is found here by watching for the player coming back to a state it has
// been in before.

#include "aychip.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ay {
namespace {

const uint16_t kTable[0x54] = {
  0xfbf, 0xedc, 0xe07, 0xd3d, 0xc7f, 0xbcc, 0xb22, 0xa82, 0x9eb, 0x95d, 0x8d6, 0x857,
  0x7df, 0x76e, 0x703, 0x69f, 0x640, 0x5e6, 0x591, 0x541, 0x4f6, 0x4ae, 0x46b, 0x42c,
  0x3f0, 0x3b7, 0x382, 0x34f, 0x320, 0x2f3, 0x2c8, 0x2a1, 0x27b, 0x257, 0x236, 0x216,
  0x1f8, 0x1dc, 0x1c1, 0x1a8, 0x190, 0x179, 0x164, 0x150, 0x13d, 0x12c, 0x11b, 0x10b,
  0xfc, 0xee, 0xe0, 0xd4, 0xc8, 0xbd, 0xb2, 0xa8, 0x9f, 0x96, 0x8d, 0x85,
  0x7e, 0x77, 0x70, 0x6a, 0x64, 0x5e, 0x59, 0x54, 0x4f, 0x4b, 0x47, 0x43,
  0x3f, 0x3b, 0x38, 0x35, 0x32, 0x2f, 0x2d, 0x2a, 0x28, 0x25, 0x23, 0x21};

const int kStack = 64;          // the original has room for 16 words per channel
const int kSteps = 4096;        // commands in one interrupt before the song counts as broken

struct Broken {};

struct Channel {
  uint16_t address = 0;         // in the channel's program
  uint16_t ornament = 0, ornament_start = 0;
  uint16_t sample = 0, sample_start = 0;
  uint16_t tone = 0;
  uint8_t volume = 0;
  uint8_t amplitude = 0;
  uint8_t sample_ticks = 0;
  uint8_t mixer = 8;
  uint8_t skip = 1;             // interrupts until the next note
  uint8_t note = 0;
  uint8_t transpose = 0;
  bool keep_sample = false;     // b0e: notes don't restart the sample
  bool sample_kept = false;     // b1e
  bool semitones = false;       // b3e: the ornament counts in notes
  std::vector<uint16_t> stack;
};

struct Player {
  const uint8_t* mem = nullptr;
  Channel ch[3];
  uint8_t noise = 0;
  uint8_t noise_mask = 31;

  uint8_t at(unsigned a) const { return mem[a & 0xFFFF]; }
  uint16_t word(unsigned a) const { return static_cast<uint16_t>(at(a) | (at(a + 1) << 8)); }

  void start(const uint8_t* memory, uint16_t address, uint8_t mask) {
    *this = Player();
    mem = memory;
    noise_mask = mask;
    for (int i = 0; i < 3; ++i) ch[i].address = word(address + i * 2);
  }

  static void push(Channel& c, uint16_t v) {
    if (c.stack.size() >= kStack) throw Broken();
    c.stack.push_back(v);
  }
  static uint16_t pop(Channel& c) {
    if (c.stack.empty()) throw Broken();
    const uint16_t v = c.stack.back();
    c.stack.pop_back();
    return v;
  }

  static void finish(Channel& c) { c.amplitude = c.tone != 0 ? c.volume & 15 : 0; }

  // first: the interrupt a note begins in, which has the note's own pitch
  void registers(Channel& c, bool first = false) {
    if (--c.sample_ticks == 0) {
      for (int steps = 0;; ++steps) {
        if (steps > kSteps) throw Broken();
        const uint8_t b = at(c.sample);
        if (b <= 0x1D) {
          c.volume = b;
          c.sample_ticks = at(c.sample + 1);
          c.sample += 2;
          break;
        }
        if (b == 0x80) {
          c.sample = word(c.sample + 1);
          continue;
        }
        c.volume = static_cast<uint8_t>(b - 0x32);
        c.sample += 1;
        c.sample_ticks = 1;
        break;
      }
    }
    if (c.tone != 0 && !first) {
      for (int steps = 0;; ++steps) {
        if (steps > kSteps) throw Broken();
        const uint8_t b = at(c.ornament);
        if (b == 0x80) { c.ornament = word(c.ornament + 1); continue; }
        if (b == 0x82) { ++c.ornament; c.semitones = true; continue; }
        if (b == 0x83) { ++c.ornament; c.semitones = false; continue; }
        if (b == 0x84) { ++c.ornament; c.mixer ^= 9; continue; }
        if (c.semitones) {
          c.note = static_cast<uint8_t>(c.note + b);
          c.tone = kTable[std::min<int>(c.note, 0x53)];
        } else {
          c.tone = static_cast<uint16_t>(c.tone + static_cast<int8_t>(b));
        }
        ++c.ornament;
        break;
      }
    }
    finish(c);
  }

  void interpret(Channel& c) {
    if (--c.skip != 0) {
      registers(c);
      return;
    }
    for (int steps = 0;; ++steps) {
      if (steps > kSteps) throw Broken();
      const uint8_t b = at(c.address);
      if (b < 0x80) {
        if (b != 0) {
          c.note = static_cast<uint8_t>(b - 1 + c.transpose);
          c.tone = kTable[std::min<int>(c.note, 0x53)];
          c.semitones = false;
        } else {
          c.tone = 0;
        }
        c.skip = at(c.address + 1);
        c.address += 2;
        c.ornament = c.ornament_start;
        if (!c.sample_kept) {
          c.sample_kept = c.keep_sample;
          c.sample = c.sample_start;
          c.volume = at(c.sample);
          c.sample_ticks = at(c.sample + 1);
          c.sample += 2;
          finish(c);
        } else {
          // (Ay_Emul runs the ornament here too; the Z80 player doesn't)
          registers(c, true);
        }
        return;
      }
      switch (b) {
        case 0x80: c.address = word(c.address + 1); break;
        case 0x81:
          push(c, static_cast<uint16_t>(c.address + 3));
          c.address = word(c.address + 1);
          break;
        case 0x82:
          push(c, at(c.address + 1));
          c.address += 2;
          push(c, c.address);
          break;
        case 0x83: {
          if (c.stack.size() < 2) throw Broken();
          uint16_t& count = c.stack[c.stack.size() - 2];
          --count;
          if ((count & 255) != 0) {
            c.address = c.stack.back();
          } else {
            c.stack.resize(c.stack.size() - 2);
            c.address += 1;
          }
          break;
        }
        case 0x84: noise = at(c.address + 1); c.address += 2; break;
        case 0x85: c.mixer = at(c.address + 1); c.address += 2; break;
        case 0x86: c.ornament_start = word(c.address + 1); c.address += 3; break;
        case 0x87: c.sample_start = word(c.address + 1); c.address += 3; break;
        case 0x88: c.transpose = at(c.address + 1); c.address += 2; break;
        case 0x89: c.address = pop(c); break;
        case 0x8A: c.address += 1; c.keep_sample = true; c.sample_kept = false; break;
        case 0x8B: c.address += 1; c.keep_sample = false; c.sample_kept = false; break;
        case 0x8C: c.address += 3; break;      // a call of Z80 code: nothing to do with the sound
        case 0x8D:
          noise = static_cast<uint8_t>((noise + at(c.address + 1)) & noise_mask);
          c.address += 2;
          break;
        case 0x8E:
          c.transpose = static_cast<uint8_t>(c.transpose + at(c.address + 1));
          c.address += 2;
          break;
        case 0x8F: push(c, c.transpose); c.address += 1; break;
        case 0x90: c.transpose = static_cast<uint8_t>(pop(c)); c.address += 1; break;
        default: c.address += 1; break;
      }
    }
  }

  void tick(Regs& r) {
    for (Channel& c : ch) interpret(c);
    for (int i = 0; i < 3; ++i) {
      const uint16_t tone = ch[i].tone & 0xFFF;
      r.r[i * 2] = tone & 0xFF;
      r.r[i * 2 + 1] = tone >> 8;
      r.r[8 + i] = ch[i].amplitude;
    }
    r.r[6] = noise & 31;
    r.r[7] = (ch[0].mixer | (ch[1].mixer << 1) | (ch[2].mixer << 2)) & 0x3F;
    r.shape_written = false;
  }

  bool audible() const {
    for (const Channel& c : ch)
      if (c.amplitude != 0 && (c.mixer & 9) != 9) return true;
    return false;
  }

  // everything the next interrupts depend on
  // (the noise's period is the first byte); with only >= 0, of that channel alone
  std::string state(int only = -1) const {
    std::string s;
    auto put = [&s](unsigned v, int bytes) {
      for (int i = 0; i < bytes; ++i) s += static_cast<char>((v >> (i * 8)) & 0xFF);
    };
    if (only < 0) put(noise, 1);
    for (const Channel& c : ch) {
      if (only >= 0 && &c != &ch[only]) continue;
      put(c.address, 2); put(c.ornament, 2); put(c.ornament_start, 2);
      put(c.sample, 2); put(c.sample_start, 2); put(c.tone, 2);
      put(c.volume, 1); put(c.sample_ticks, 1); put(c.mixer, 1); put(c.skip, 1);
      put(c.note, 1); put(c.transpose, 1);
      put((c.keep_sample ? 1 : 0) | (c.sample_kept ? 2 : 0) | (c.semitones ? 4 : 0), 1);
      put(static_cast<unsigned>(c.stack.size()), 1);
      for (uint16_t v : c.stack) put(v, 2);
    }
    return s;
  }
};

std::string text_at(const std::vector<uint8_t>& d, long at) {
  std::string s;
  if (at < 0) return s;
  for (size_t i = static_cast<size_t>(at); i < d.size() && d[i] != 0 && s.size() < 1024; ++i)
    s += static_cast<char>(d[i]);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

long be16(const std::vector<uint8_t>& d, long at) {
  if (at < 0 || static_cast<size_t>(at) + 2 > d.size()) throw Refused("the file ends too early");
  return (d[at] << 8) | d[at + 1];
}

// an offset in an AY file: signed, from the place where it stands
long relative(const std::vector<uint8_t>& d, long at) {
  const long v = be16(d, at);
  return at + (v >= 0x8000 ? v - 0x10000 : v);
}

const double kFrameHz = 50.0;
const long kLongest = static_cast<long>(30 * 60 * kFrameHz);     // interrupts

class Fxm : public FrameEngine {
 public:
  void open(const std::string&, const std::vector<uint8_t>& data, int track, int rate,
            Info& info) override {
    mem_.assign(65536, 0);
    long body = 0;            // where the three pointers are in the file
    uint16_t address = 0;
    uint8_t mask = 31;
    long said = 0;            // the length as the file says it, in interrupts
    if (data.size() >= 12 && std::memcmp(data.data(), "FXSM", 4) == 0) {
      if (track != 0) throw Refused("no such track");
      address = static_cast<uint16_t>(data[4] | (data[5] << 8));
      body = 6;
      info.type = "Fuxoft AY Language";
      info.tracks = 1;
    } else if (data.size() >= 20 && std::memcmp(data.data(), "ZXAYAMAD", 8) == 0) {
      info.author = text_at(data, relative(data, 12));
      info.comment = text_at(data, relative(data, 14));
      info.tracks = data[16] + 1;
      if (track >= info.tracks) throw Refused("no such track");
      const long songs = relative(data, 18);
      const long entry = songs + track * 4;
      info.title = text_at(data, relative(data, entry));
      const long song = relative(data, entry + 2);
      address = static_cast<uint16_t>(be16(data, song));
      if (static_cast<size_t>(song) + 14 > data.size()) throw Refused("the file ends too early");
      mask = data[song + 2];
      said = static_cast<long>(data[song + 3]) * be16(data, song + 4);
      body = song + 14;
      info.type = "Fuxoft AY Language (Amadeus)";
    } else {
      throw Refused("not a Fuxoft AY Language song");
    }
    if (static_cast<size_t>(body) + 6 >= data.size()) throw Refused("the song has no data");
    // the three pointers have to be inside the Spectrum's memory
    if (address > 0x10000 - 6) throw Refused("not a Fuxoft AY Language song: it is loaded past the end of the memory");
    const size_t length = std::min<size_t>(data.size() - body, 65536 - address);
    std::memcpy(mem_.data() + address, data.data() + body, length);
    // the three programs have to begin inside the data
    for (int i = 0; i < 3; ++i) {
      const unsigned p = mem_[address + i * 2] | (mem_[address + i * 2 + 1] << 8);
      if (p < static_cast<unsigned>(address) + 6 || p >= address + length)
        throw Refused("not a Fuxoft AY Language song: a channel begins outside the data");
    }

    // once through without sound, for the end
    Player p;
    p.start(mem_.data(), address, mask);
    std::unordered_map<std::string, long> seen, seen_but_noise;
    long last_sound = -1, loop_from = -1, loop_to = -1;
    long notes_from = -1, notes_to = -1;       // the same, not looking at the noise's period
    std::unordered_map<std::string, long> seen_channel[3];
    long channel_from[3] = {-1, -1, -1}, channel_to[3] = {-1, -1, -1};
    std::vector<uint8_t> sounding;
    try {
      for (long t = 0; t < kLongest; ++t) {
        const std::string state = p.state();
        const auto found = seen.emplace(state, t);
        if (!found.second) {
          loop_from = found.first->second;
          loop_to = t;
          break;
        }
        if (notes_to < 0) {
          const auto notes = seen_but_noise.emplace(state.substr(1), t);
          if (!notes.second) {
            notes_from = notes.first->second;
            notes_to = t;
          }
        }
        for (int c = 0; c < 3; ++c) {
          if (channel_to[c] >= 0) continue;
          const auto one = seen_channel[c].emplace(p.state(c), t);
          if (!one.second) {
            channel_from[c] = one.first->second;
            channel_to[c] = t;
          }
        }
        Regs r;
        p.tick(r);
        const bool a = p.audible();
        sounding.push_back(a);
        if (a) last_sound = t;
      }
    } catch (const Broken&) {
      throw Refused("the song's data is broken");
    }
    if (last_sound < 0) throw Refused("the song is silent");
    if (loop_to < 0 && notes_to >= 0) {
      // The notes come round, but the noise is never the same again when they do
      // (songs that add to the noise's period): the notes decide.
      loop_from = notes_from;
      loop_to = notes_to;
    }
    if (loop_to < 0) {
      // The channels come round one by one but not together within half an hour
      // (one of them plays variations). The song is as long as the round that two
      // channels share, else as the longest round.
      long period[3], best = -1;
      for (int c = 0; c < 3; ++c) period[c] = channel_to[c] >= 0 ? channel_to[c] - channel_from[c] : -1;
      for (int c = 0; c < 3 && best < 0; ++c)
        for (int o = c + 1; o < 3; ++o)
          if (period[c] > 1 && period[c] == period[o]) best = c;
      if (best < 0)
        for (int c = 0; c < 3; ++c)
          if (period[c] > 1 && (best < 0 || period[c] > period[best])) best = c;
      if (best >= 0) {
        loop_from = channel_from[best];
        loop_to = channel_to[best];
        for (long t = loop_from; t < loop_to; ++t) sounding[t] = 1;   // not a silent end
      }
    }
    if (loop_to >= 0) {
      bool silent_loop = true;
      for (long t = loop_from; t < loop_to && silent_loop; ++t) silent_loop = !sounding[t];
      // a song that ends in a silent loop has an end; others are played up to
      // where they begin again
      length_ = silent_loop ? std::min(loop_to, last_sound + 1 + static_cast<long>(kFrameHz / 2)) : loop_to;
    } else {
      // the channels' loops never meet: what the file says, or five minutes
      length_ = said > 0 ? said : static_cast<long>(5 * 60 * kFrameHz);
    }
    info.duration_ms = static_cast<long>(length_ * 1000.0 / kFrameHz);

    player_.start(mem_.data(), address, mask);
    // a Spectrum 128: 1.7734 MHz at the chip, 50 interrupts per second
    setup(rate, kFrameHz, 1773400.0, Chip::AY, Layout::ABC);
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
  Player player_;
  long length_ = 0, played_ = 0;
};

}  // namespace

std::unique_ptr<Engine> make_fxm() {
  return std::make_unique<Fxm>();
}

}  // namespace ay
