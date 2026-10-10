// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-amiga: Jesper Olsen's game music (.jo): the songs of LollyPop, Georg Glaxo and
// Harald Hardtand.
//
// The replay routines of these games may not be shipped, so this one was written for
// this player from a description of what the games' drivers do with their songs (which
// value goes to which register of the sound chip, and when), without the original
// code: a clean room. The sound chip is ScummVM's Paula (audio/mods/paula.cpp, GPL 3
// or later), which the build fetches; the headers in maxtrax_shim/ stand in for the
// rest of ScummVM.
//
// Three drivers:
//   L  LollyPop      a table of offsets to start lists, voice records in the song
//   G  Georg Glaxo   the same driver, a little older (other tempo counting, no IFF
//                    samples); its songs look like L's and are known by a hash
//   H  Harald Hardtand  an older, smaller relative with its own layout; the songs
//                    begin with the words 0000 xxxx 0001 7FFF
// Songs that begin with 6000 carry their own replay routine (a whole program): they
// are not for this engine.
//
// The driver plays directly in the song: its voice records are areas of the file that
// it changes while it plays. So it works on a copy of the file, and every read and
// write of that copy goes through a check: a damaged file reads zeros outside itself
// and its writes outside are dropped.
//
// For testing: with BBMP_JO_LOG=<file> the engine first writes every register write of
// the song (until its end) to <file>, in the format of the clean-room description.

#include "engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "common/scummsys.h"
#include "audio/mixer.h"
#include "audio/mods/paula.h"

// Paula asks the system for its mixer's lock. The MaxTrax engine has the same object;
// when both are built in, its definition is taken and this one is left unused.
static OSystem jo_system;
__attribute__((weak)) OSystem *g_system = &jo_system;

namespace {

// --- the song's memory --------------------------------------------------------------

// The song, as the driver sees it. Addresses are offsets from the start of the song;
// anything outside reads as 0 and cannot be written.
struct Memory {
  std::vector<uint8_t> d;  // the song, then zeros (for Paula: samples may run past the end)
  int64_t size = 0;        // the song's own length

  uint8_t r8(int64_t a) const { return a >= 0 && a < size ? d[static_cast<size_t>(a)] : 0; }
  uint16_t r16(int64_t a) const { return static_cast<uint16_t>((r8(a) << 8) | r8(a + 1)); }
  uint32_t r32(int64_t a) const { return (uint32_t(r16(a)) << 16) | r16(a + 2); }
  void w8(int64_t a, unsigned v) {
    if (a >= 0 && a < size) d[static_cast<size_t>(a)] = static_cast<uint8_t>(v);
  }
  void w16(int64_t a, unsigned v) { w8(a, v >> 8); w8(a + 1, v); }
  void w32(int64_t a, uint32_t v) { w16(a, v >> 16); w16(a + 2, v); }
};

int8_t s8(unsigned v) { return static_cast<int8_t>(static_cast<uint8_t>(v)); }
int16_t s16(unsigned v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

// Room behind the song for the longest sample Paula can be told to play (65,536 words).
const int64_t kPad = 131072 + 16;

// --- Paula ----------------------------------------------------------------------------

// ScummVM's Paula, driven from outside: the driver's register writes are passed on as
// they come. LC and LEN are latched and taken when the channel is switched on or when
// its sample next starts over, as on the Amiga.
class JoPaula : public Audio::Paula {
 public:
  JoPaula(int rate, const uint8_t* mem, int64_t song, int64_t total, FilterMode filter)
      : Paula(true, rate, static_cast<uint>(rate), filter), mem_(mem), song_(song), total_(total) {
    for (int c = 0; c < 4; ++c) {
      // as UADE leaves Paula before the song starts: off, volume 0, period 150, one
      // word of silence
      lc_[c] = -1;
      len_[c] = 1;
      on_[c] = false;
      latch(c);
      setChannelPeriod(static_cast<byte>(c), 150);
      setChannelVolume(static_cast<byte>(c), 0);
    }
    startPlay();
  }
  void interrupt() override {}

  // Stereo as UADE mixes it: <panning> 0 = channels fully apart, 1 = mono; each
  // channel puts <panning>/2 of itself on the other side. Channels 0 and 3 are on the
  // left, 1 and 2 on the right, as on the Amiga.
  void pan(double panning) {
    const int other = static_cast<int>(255 * std::max(0.0, std::min(1.0, panning)) / 2 + 0.5);
    setChannelPanning(0, static_cast<byte>(other));
    setChannelPanning(1, static_cast<byte>(255 - other));
    setChannelPanning(2, static_cast<byte>(255 - other));
    setChannelPanning(3, static_cast<byte>(other));
  }
  void lc(int c, int64_t at) { lc_[c] = at; latch(c); }
  void len(int c, unsigned words) { len_[c] = words ? words : 65536; latch(c); }
  void per(int c, unsigned period) {
    // the chip cannot fetch faster than this; 0 and very long periods are silence-like
    int p = static_cast<int>(period);
    if (p < 113) p = 113;
    if (p > 32767) p = 32767;
    setChannelPeriod(static_cast<byte>(c), static_cast<int16>(p));
  }
  void vol(int c, unsigned v) { setChannelVolume(static_cast<byte>(c), static_cast<byte>(std::min(v & 127u, 64u))); }
  void dmacon(unsigned v) {
    for (int c = 0; c < 4; ++c) {
      if (!(v & (1u << c))) continue;
      if (v & 0x8000) {
        if (!on_[c]) enableChannel(static_cast<byte>(c));
        on_[c] = true;
      } else {
        disableChannel(static_cast<byte>(c));
        on_[c] = false;
      }
    }
  }
  void filter(bool on) { setAudioFilter(on); }

 private:
  // Where the latched sample is, kept inside the memory: a pointer outside the song
  // plays the zeros behind it, and no sample reaches past the end of the zeros.
  void latch(int c) {
    int64_t at = lc_[c];
    if (at < 0 || at >= song_) at = song_;
    int64_t bytes = 2 * static_cast<int64_t>(len_[c]);
    if (bytes > total_ - at) bytes = total_ - at;
    setChannelSampleStart(static_cast<byte>(c), reinterpret_cast<const int8*>(mem_ + at));
    setChannelSampleLen(static_cast<byte>(c), static_cast<uint32>(bytes / 2));
  }

  const uint8_t* mem_;
  int64_t song_, total_;
  int64_t lc_[4];
  unsigned len_[4];
  bool on_[4];
};

// --- the drivers ------------------------------------------------------------------------

enum Kind { kL, kG, kH };

const uint16_t kPeriods[95] = {
    27360, 25824, 24384, 23008, 21712, 20496, 19344, 18256, 17232, 16272, 15360, 14496,
    13680, 12912, 12192, 11504, 10856, 10248, 9672,  9128,  8616,  8136,  7680,  7248,
    6840,  6456,  6096,  5752,  5428,  5124,  4836,  4564,  4308,  4068,  3840,  3624,
    3420,  3228,  3048,  2876,  2714,  2562,  2418,  2282,  2154,  2034,  1920,  1812,
    1710,  1614,  1524,  1438,  1357,  1281,  1209,  1141,  1077,  1017,  960,   906,
    855,   807,   762,   719,   679,   641,   605,   571,   539,   508,   480,   453,
    428,   404,   381,   360,   339,   320,   302,   285,   269,   254,   240,   226,
    214,   202,   190,   180,   170,   160,   151,   143,   135,   127,   120};

// L/G's volume "product": a times b over 63, by the drivers' two tables, including
// what they give for values above 63.
unsigned product(unsigned a, unsigned b) {
  a &= 0x7F;
  b &= 0xFF;
  const unsigned row = a < 64 ? 64 * a : a == 127 ? 1 : 0;
  const unsigned at = (row & 0xFF00) | ((row + b) & 0xFF);
  return ((at >> 6) * (at & 63)) / 63;
}

// ten minutes: a song that has not ended by then is stopped (a fuzzing harness may
// define less, to get through more runs)
#ifndef JO_MAX_TICKS
#define JO_MAX_TICKS (600 * 50)
#endif
const int kMaxTicks = JO_MAX_TICKS;

class Driver {
 public:
  Driver(Kind kind, const std::vector<uint8_t>& file) : kind_(kind) {
    m_.size = static_cast<int64_t>(file.size());
    m_.d.assign(file.begin(), file.end());
    m_.d.resize(file.size() + static_cast<size_t>(kPad), 0);
  }

  Memory& memory() { return m_; }
  void attach(JoPaula* paula) { paula_ = paula; }
  void log_to(FILE* f) { log_ = f; }

  // The host's start calls for subsong <track> (0 = the first).
  void start(int track) {
    for (bool& f : flag_) f = false;
    if (kind_ == kH) {
      call(static_cast<unsigned>(track) & 0xFFFF);
    } else {
      call(0x5000);
      call(0x4000 | ((static_cast<unsigned>(track) + 1) & 0xFF));
    }
    for (int c = 0; c < 4; ++c) copy_[c] = flag_[c];
  }

  // One call of the play entry. True when the song has ended during it.
  bool tick() {
    ended_ = false;
    if (log_) std::fprintf(log_, "T %ld\n", ticks_);
    ++ticks_;
    if (kind_ == kH) tick_h(); else tick_lg();
    if (ended_ && log_) std::fprintf(log_, "END\n");
    return ended_;
  }

 private:
  // --- register writes
  void out_lc(int c, int64_t at) {
    if (log_) std::fprintf(log_, "%dLC S+%06x\n", c, static_cast<unsigned>(at) & 0xFFFFFF);
    if (paula_) paula_->lc(c, at);
  }
  void out_len(int c, unsigned v) {
    if (log_) std::fprintf(log_, "%dLEN %04x\n", c, v & 0xFFFF);
    if (paula_) paula_->len(c, v & 0xFFFF);
  }
  void out_per(int c, unsigned v) {
    if (log_) std::fprintf(log_, "%dPER %04x\n", c, v & 0xFFFF);
    if (paula_) paula_->per(c, v & 0xFFFF);
  }
  void out_vol(int c, unsigned v) {
    v &= 127;  // the host's volume control at full volume keeps the low 7 bits
    if (log_) std::fprintf(log_, "%dVOL %04x\n", c, v);
    if (paula_) paula_->vol(c, v);
  }
  void out_dma(unsigned v) {
    if (log_) std::fprintf(log_, "DMACON %04x\n", v & 0xFFFF);
    if (paula_) paula_->dmacon(v & 0xFFFF);
  }
  void out_led(int b) {
    if (log_) std::fprintf(log_, "LED %d\n", b);
    if (paula_) paula_->filter(b == 0);
  }

  // A voice's sequence has come to its end or its loop: when all voices that had a
  // sequence have done so, the song is over (once; the count then starts again).
  void voice_done(int c) {
    flag_[c] = false;
    if (flag_[0] || flag_[1] || flag_[2] || flag_[3]) return;
    ended_ = true;
    for (int i = 0; i < 4; ++i) flag_[i] = copy_[i];
  }

  void call(unsigned v) {
    if (log_) std::fprintf(log_, "I %04x\n", v);
    if (kind_ == kH) start_h(v);
    else command_lg(v);
  }

  // --- L and G -----------------------------------------------------------------------

  void command_lg(unsigned v) {
    if (v & 0x8000) return;  // effects for the games: not used by the songs
    if (v & 0x100) { master_ = v & 0xFF; return; }
    if (kind_ == kL && (v & 0x800)) return;  // save/restore of the state: not used
    if (v & 0x4000) start_list(v & 0xFF);
  }

  void start_list(unsigned s) {
    master_ = 63;
    dma_off_ = 0;
    dma_on_ = 0x8000;
    int64_t e = m_.r16(2 * static_cast<int64_t>(s));
    for (int n = 0; n < 4096 && e < m_.size && m_.r16(e) != 0x7FFF; ++n, e += 10) {
      const int64_t r = s16(m_.r16(e));
      const unsigned table = m_.r16(e + 2), seq = m_.r16(e + 4);
      const int c = m_.r8(e + 6) & 3;
      const unsigned tempo = m_.r8(e + 7), prio = m_.r8(e + 8), vol = m_.r8(e + 9);
      if (prio < m_.r8(r + 0x40)) continue;
      m_.w8(r + 0x40, prio);
      voice_[c] = r;
      has_[c] = true;
      m_.w32(r + 0x2E, static_cast<uint32_t>(static_cast<int32_t>(s16(table))));
      m_.w16(r + 0x34, seq);
      if (seq) flag_[c] = true;
      out_dma(1u << c);
      m_.w16(r + 0x38, (1u << c) + 0x8000);
      m_.w16(r + 0x3A, 1u << c);
      m_.w32(r + 0x3C, static_cast<uint32_t>(c));
      m_.w8(r + 0x03, tempo);
      m_.w8(r + 0x40, seq ? prio : 0);
      m_.w8(r + 0x17, vol);
      m_.w16(r + 0x14, 63);
      m_.w16(r + 0x12, 63);
      m_.w16(r + 0x32, kind_ == kL ? 0x7F01 : 0x0001);
      m_.w8(r + 0x43, s);
      for (int at : {0x36, 0x06, 0x08, 0x0A, 0x20, 0x0C}) m_.w16(r + at, 0);
      m_.w8(r + 0x41, kind_ == kL ? 0xFF : 0);
      if (kind_ == kL)
        for (int at : {0x10, 0x18, 0x1A, 0x1C}) m_.w16(r + at, 0);
    }
  }

  // the instrument a voice plays: entry R$01 of its instrument table
  int64_t instrument(int64_t r) const {
    const int64_t table = static_cast<int32_t>(m_.r32(r + 0x2E));
    return m_.r16(table + 2 * static_cast<int64_t>(m_.r8(r + 1)));
  }

  // Carries out the "set word" commands at <p>: a word whose first byte has bit 7 set
  // goes into the voice record at (first byte - <minus>, or first byte and <mask>).
  // Returns where they end.
  int64_t commands(int64_t r, int64_t p, unsigned minus, unsigned mask) {
    for (int n = 0; n < 32768 && (m_.r8(p) & 0x80); ++n, p += 2) {
      const unsigned f = m_.r8(p);
      m_.w16(r + ((f & mask) - minus), m_.r16(p));
    }
    return p;
  }

  void tick_lg() {
    out_dma(dma_off_);
    dma_off_ = 0;
    dma_on_ = 0;
    for (int c = 0; c < 4; ++c) {
      if (!has_[c]) continue;
      const int64_t r = voice_[c];
      if (!m_.r16(r + 0x34)) continue;
      bool due;
      if (kind_ == kL) {
        const unsigned sum = m_.r8(r + 0x41) + m_.r8(r + 0x03);
        m_.w8(r + 0x41, sum);
        due = sum > 0xFF;
      } else {
        const unsigned t = m_.r8(r + 0x03);
        due = false;
        if (!(t & 0x80)) {
          const unsigned k = (m_.r8(r + 0x41) - 1) & 0xFF;
          m_.w8(r + 0x41, k);
          if (s8(k) < 0) { due = true; m_.w8(r + 0x41, t & 0x70); m_.w8(r + 0x03, t ^ 0x80); }
        } else {
          const unsigned k = (m_.r8(r + 0x41) - 0x10) & 0xFF;
          m_.w8(r + 0x41, k);
          if (s8(k) < 0) { due = true; m_.w8(r + 0x41, t & 7); m_.w8(r + 0x03, t ^ 0x80); }
        }
      }
      if (due) {
        const unsigned left = ((m_.r8(r + 0x33) & 0x7F) - 1) & 0xFF;
        m_.w8(r + 0x33, left);
        if (!left) { row_lg(c, r); continue; }
      }
      sound_lg(c, r);
    }
    out_dma(dma_on_ | 0x8000);
  }

  void row_lg(int c, int64_t r) {
    int64_t p = s16(m_.r16(r + 0x34));
    const unsigned w = m_.r16(p);
    if (w == 0x7FFF) {
      m_.w16(r + 0x34, 0);
      out_vol(c, 0);
      voice_done(c);
      m_.w8(r + 0x40, 0);
      return;
    }
    if (w == 0x7FFE) {
      const unsigned x = m_.r16(p + 2);
      m_.w16(r + 0x34, x);
      voice_done(c);
      p = s16(x);
    }
    p = commands(r, p, 0x80, 0xFF);
    m_.w16(r + 0x34, static_cast<unsigned>(p));
    const int64_t pattern = s16(m_.r16(p));
    const int64_t row = pattern + s16(m_.r16(r + 0x36));
    m_.w16(r + 0x0C, 0);
    int64_t q = commands(r, row, 0x80, 0xFF);
    const unsigned a = m_.r8(q), b = m_.r8(q + 1);
    q += 2;
    if (a == 0x7E || a == 0x7F) {
      m_.w8(r + 0x04, a);
      m_.w8(r + 0x05, b);
      m_.w8(r + 0x33, b);
      if (a == 0x7F) {
        m_.w8(r + 0x32, 0x7F);
        dma_off_ |= m_.r16(r + 0x3A);
      } else {
        m_.w8(r + 0x04, 0);
        m_.w8(r + 0x05, b & 0x80);
      }
    } else {
      if (!(b & 0x80)) {
        m_.w8(r + 0x42, 0xFF);
        const int64_t ins = instrument(r);
        for (int i = 0; i < 12; ++i) m_.w8(r + 0x22 + i, m_.r8(ins + i));
        m_.w16(r + 0x20, 0);
        const unsigned flags = m_.r8(ins + 0x0D);
        const unsigned bit = m_.r16(r + 0x3A);
        if (kind_ == kL) {
          bool go = true;
          if (flags & 4) {
            if (m_.r8(r + 0x1C)) go = false;
            else m_.w8(r + 0x1C, 0xFF);
          } else {
            m_.w8(r + 0x1C, 0);
          }
          if (go) {
            dma_off_ |= bit;
            if (!(flags & 1)) out_dma(bit);
          }
        } else {
          if (flags & 1) out_dma(bit);
          else dma_off_ |= bit;
        }
      }
      m_.w8(r + 0x32, a);
      m_.w8(r + 0x33, b);
      if (kind_ == kL) {
        m_.w8(r + 0x32, a + m_.r8(r + 0x0B));
        m_.w16(r + 0x20, 0);
      }
    }
    m_.w16(r + 0x36, static_cast<unsigned>(q - pattern));
    if (!m_.r16(q)) {
      m_.w16(r + 0x36, 0);
      const unsigned k = (m_.r8(r + 0x07) - 1) & 0xFF;
      m_.w8(r + 0x07, k);
      if (s8(k) < 0) {
        m_.w8(r + 0x07, 0);
        m_.w16(r + 0x34, m_.r16(r + 0x34) + 2);
      }
    }
  }

  // the offset of a waveform's part for instruments with flag bit 7
  int64_t wave(int64_t r, unsigned flags, unsigned length) const {
    if (!(flags & 0x80)) return 0;
    return s16(2u * m_.r8(r + 0x1F) * length);
  }

  void sound_lg(int c, int64_t r) {
    if (m_.r8(r + 0x32) == 0x7F) return;
    dma_on_ |= m_.r16(r + 0x3A);
    const int64_t ins = instrument(r);
    if (kind_ == kG) { m_.w16(r + 0x0E, 0); m_.w16(r + 0x08, 0); }
    const uint32_t sample = m_.r32(ins + 0x0E);
    const bool iff = kind_ == kL && (sample & 0x80000000u);
    const int64_t iff_at = sample & 0x7FFFFFFF;
    if (m_.r8(r + 0x42)) {
      if (kind_ == kL) {
        m_.w16(r + 0x0E, 0);
        m_.w16(r + 0x08, 0);
        m_.w8(r + 0x42, 0);
        m_.w8(r + 0x05, 0xFF);
        const unsigned flags = m_.r8(ins + 0x0D);
        if (flags & 2) m_.w8(r + 0x1F, 0);
        if (iff) {
          out_lc(c, iff_at);
          out_len(c, ((m_.r16(iff_at - 82) + m_.r16(iff_at - 78)) & 0xFFFF) >> 1);
        } else {
          const unsigned length = m_.r16(ins + 0x12);
          out_lc(c, static_cast<int64_t>(sample) + wave(r, flags, length));
          out_len(c, length);
        }
      } else {
        out_lc(c, sample);
        out_len(c, m_.r16(ins + 0x12));
        m_.w8(r + 0x42, 0);
        m_.w8(r + 0x05, 0xFF);
      }
    } else {
      const unsigned flags = m_.r8(ins + 0x0D);
      if (iff) {
        out_len(c, m_.r16(iff_at - 78) >> 1);
        out_lc(c, iff_at + s16(m_.r16(iff_at - 82)));
      } else {
        const unsigned length = m_.r16(ins + (kind_ == kL ? 0x12 : 0x18));
        out_lc(c, static_cast<int64_t>(m_.r32(ins + 0x14)) + wave(r, flags, length));
        out_len(c, m_.r16(ins + 0x18));
      }
    }
    program_lg(r, 0x22);
    program_lg(r, 0x26);
    program_lg(r, 0x2A);
    m_.w16(r + 0x20, m_.r16(r + 0x20) + s8(m_.r8(r + 0x0D)));

    unsigned v = product(m_.r8(r + 0x17), m_.r8(r + 0x15));
    v = product(v, m_.r8(r + 0x13));
    v = product(v, master_);
    v = product(v, m_.r8(ins + 0x0C));
    out_vol(c, v);

    unsigned n = m_.r8(r + 0x09);
    if (s8(n) >= 0) n = (n + (kind_ == kG ? m_.r8(r + 0x0B) : 0) + m_.r8(r + 0x32)) & 0xFF;
    const unsigned index = n & 0x7F;
    const unsigned period = index < 95 ? kPeriods[index] : 64 * (index - 95);
    const int bend = (m_.r8(r + 0x19) & 1) ? 0 : s8(m_.r8(r + 0x0F));
    out_per(c, period - s8(m_.r8(r + 0x11)) - bend - m_.r16(r + 0x20));
  }

  // One of a voice's three little programs (A at $22, B at $26, C at $2A).
  void program_lg(int64_t r, int at) {
    const int64_t s = r + at;
    unsigned p = m_.r16(s);
    if (!p) return;
    const unsigned count = (m_.r8(s + 2) - 1) & 0xFF;
    m_.w8(s + 2, count);
    if (s8(count) >= 0) return;
    m_.w8(s + 2, 0);
    int64_t q = s16(p);
    unsigned i = m_.r8(q), v = m_.r8(q + 1);
    if (i == 0x7E) {
      if (!(v & 0x80)) {  // a wait
        m_.w8(s + 3, v);
        m_.w16(s, p + 2);
        m_.w8(s + 2, v);
      } else if (!(m_.r8(r + 0x05) & 0x80)) {  // a hold point, the key let go of
        m_.w16(s, p + 2);
        m_.w8(s + 2, m_.r8(s + 3));
      }
      return;
    }
    const unsigned word = (i << 8) | v;
    if (word == 0x7FFF) return;
    if (word == 0x7FFE) {
      p = m_.r16(q + 2);
      q = s16(p);
      i = m_.r8(q);
      v = m_.r8(q + 1);
    }
    m_.w8(r + (i & 0x7F) + 1, v);
    m_.w16(s, p + 2);
    m_.w8(s + 2, m_.r8(s + 3));
  }

  // --- H -----------------------------------------------------------------------------

  void start_h(unsigned s) {
    const int64_t rec = 8 + 26 * static_cast<int64_t>(s & 0xFF);
    for (int i = 0; i < 4; ++i) {
      hv_[i] = m_.r32(rec + 10 + 4 * i);
      has_[i] = true;
    }
    g3_ = 0xFF;
    g0_ = m_.r8(rec + 8);
    out_led(1);
    if (m_.r8(rec + 9) & 0x80) out_led(0);
    for (int i = 0; i < 4; ++i) {
      const int64_t r = hv_[i];
      const unsigned seq = m_.r16(rec + 2 * i);
      for (int b = 0; b < 122; ++b) m_.w8(r + b, 0);
      m_.w16(r + 0x02, 2);
      m_.w16(r + 0x00, 0);
      m_.w16(r + 0x04, seq > 1 ? seq : 0);
      if (seq > 1) flag_[3 - i] = true;
    }
    out_dma(0x000F);
    for (int i = 0; i < 4; ++i) m_.w16(hv_[i] + 0x58, m_.r8(rec + 9) & 0x7F);
  }

  void tick_h() {
    if (!g3_) return;
    g1_ = (g1_ - 1) & 0xFF;
    if (s8(g1_) < 0) {
      if (!g2_) { g1_ = g0_ & 0x0F; g2_ = 0xFF; }
      else { g1_ = g0_ >> 4; g2_ = 0; }
    }
    const bool due = g1_ == 0;
    for (int i = 0; i < 4; ++i) {
      const int64_t r = hv_[i];
      const int c = 3 - i;
      const unsigned w = m_.r16(r + 0x04);
      if (!w) continue;
      if (!(w & 0x8000) && !due) { sound_h(c, r); continue; }
      const unsigned left = ((m_.r8(r + 0x03) & 0x7F) - 1) & 0xFF;
      m_.w8(r + 0x03, left);
      if (left) { sound_h(c, r); continue; }
      row_h(c, r, w);
    }
  }

  void row_h(int c, int64_t r, unsigned w) {
    const unsigned keep = w & 0x8000;
    int64_t p = w & 0x7FFF;
    const unsigned first = m_.r16(p);
    if (first == 0x7FFF) {
      m_.w16(r + 0x04, 0);
      out_vol(c, 0);
      voice_done(c);
      return;
    }
    if (first == 0x7FFE) {
      const unsigned x = m_.r16(p + 2);
      m_.w16(r + 0x04, x | keep);
      voice_done(c);
      p = x & 0x7FFF;
    }
    p = commands(r, p, 0x2C, 0xFF);
    m_.w16(r + 0x04, static_cast<unsigned>(p) | keep);
    const int64_t pattern = s16(m_.r16(p));
    const int64_t row = pattern + s16(m_.r16(r + 0x06));
    m_.w16(r + 0x50, 16);
    m_.w16(r + 0x40, 0);
    int64_t q = commands(r, row, 0, 0x70);
    m_.w16(r + 0x00, m_.r16(r + 0x00) & 0x7FFF);
    m_.w16(r + 0x50, m_.r16(r + 0x50) & 0x1F);
    m_.w16(r + 0x58, m_.r16(r + 0x58) & 0x2F);
    const unsigned a = m_.r8(q), b = m_.r8(q + 1);
    q += 2;
    if (a >= 0x7E) {
      m_.w8(r + 0x52, a);
      m_.w8(r + 0x53, b);
      m_.w8(r + 0x03, b);
      if (a == 0x7F) {
        m_.w8(r + 0x02, 0x7F);
        out_dma(1u << c);
      } else {
        m_.w16(r + 0x52, m_.r16(r + 0x52) & 0x80);
      }
    } else {
      const int64_t ins = s16(m_.r16(r + 0x00));
      m_.w16(r + 0x0E, 0);
      if (!(b & 0x80)) {
        out_dma(1u << c);
        m_.w16(r + 0x08, m_.r16(r + 0x00) + 8);
        m_.w16(r + 0x0C, m_.r16(ins + 2));
        m_.w16(r + 0x0A, m_.r16(ins + 0));
        m_.w16(r + 0x62, 0);
        m_.w8(r + 0x68, 0);
        m_.w8(r + 0x69, 0);
        m_.w8(r + 0x53, 0xFF);
        m_.w16(r + 0x5E, m_.r16(ins + 4));
        m_.w8(r + 0x67, m_.r8(ins + 6));
      }
      m_.w8(r + 0x02, a);
      m_.w8(r + 0x03, b);
    }
    m_.w16(r + 0x06, static_cast<unsigned>(q - pattern));
    if (!m_.r16(q)) {
      m_.w16(r + 0x06, 0);
      const unsigned k = (m_.r8(r + 0x57) - 1) & 0xFF;
      m_.w8(r + 0x57, k);
      if (s8(k) < 0) {
        m_.w8(r + 0x57, 0);
        m_.w16(r + 0x04, m_.r16(r + 0x04) + 2);
      }
    }
  }

  // The item a program's position <at> (a record word) points at, after following its
  // jumps (7FFE x: go on at x); false for the end mark 7FFF or a chain of jumps.
  bool item_h(int64_t r, int at, int64_t& q) {
    for (int n = 0; n < 16; ++n) {
      q = s16(m_.r16(r + at));
      const unsigned w = m_.r16(q);
      if (w == 0x7FFF) return false;
      if (w != 0x7FFE) return true;
      m_.w16(r + at, m_.r16(q + 2));
    }
    return false;
  }

  void sound_h(int c, int64_t r) {
    if (m_.r8(r + 0x02) == 0x7F) return;
    if (m_.r8(r + 0x5A) == 0x86) g0_ = m_.r8(r + 0x5B);
    const int64_t ins = s16(m_.r16(r + 0x00));
    int d = 0;

    // 1. slide, or program W
    if (m_.r8(r + 0x41)) {
      m_.w16(r + 0x0E, m_.r16(r + 0x0E) + s8(m_.r8(r + 0x41)));
    } else if (m_.r16(r + 0x5E) && !(m_.r8(r + 0x31) & 2)) {
      bool go = true;
      if (!(m_.r8(r + 0x31) & 1)) {
        const unsigned k = (m_.r8(r + 0x67) - 1) & 0xFF;
        m_.w8(r + 0x67, k);
        if (s8(k) >= 0) go = false;
        else m_.w8(r + 0x67, 0);
      }
      if (go) {
        m_.w16(r + 0x62, m_.r16(r + 0x62) & 0x0F00);
        const unsigned k = (m_.r8(r + 0x62) - 1) & 0xFF;
        m_.w8(r + 0x62, k);
        if (s8(k) >= 0) {
          m_.w16(r + 0x5E, m_.r16(r + 0x5E) + 2);
          d = s8(m_.r8(r + 0x63));
        } else {
          int64_t q = s16(m_.r16(r + 0x5E));
          unsigned e = m_.r16(q);
          if (e == 0x7FFF) {
            m_.w16(r + 0x5E, m_.r16(r + 0x5E) + 2);
            d = 0;
          } else {
            if (e == 0x7FFE) {
              const unsigned x = m_.r16(q + 2);
              m_.w16(r + 0x5E, x);
              e = m_.r16(s16(x));
            }
            m_.w16(r + 0x62, e);
            if ((e & 0x8000) && m_.r8(r + 0x53)) {
              d = 0;
            } else {
              m_.w16(r + 0x5E, m_.r16(r + 0x5E) + 2);
              d = s8(m_.r8(r + 0x63));
            }
          }
        }
      }
    }

    // 2. volume, with program V
    unsigned v = m_.r8(ins + 7) & 0x3F;
    if (m_.r16(r + 0x0C)) {
      const unsigned k = (m_.r8(r + 0x68) - 1) & 0xFF;
      m_.w8(r + 0x68, k);
      int64_t q;
      if (s8(k) < 0 && item_h(r, 0x0C, q)) {
        const unsigned f = m_.r8(q);
        m_.w8(r + 0x65, m_.r8(q + 1));
        m_.w8(r + 0x68, f);
        if (f & 0x80) {
          m_.w8(r + 0x68, f & 0x7F);
          if (!m_.r8(r + 0x53)) m_.w16(r + 0x0C, m_.r16(r + 0x0C) + 2);
        } else {
          m_.w16(r + 0x0C, m_.r16(r + 0x0C) + 2);
        }
      }
      if (m_.r16(r + 0x0C)) v = ((v * m_.r16(r + 0x64)) & 0xFFFF) >> 5;
    }
    v = ((v * static_cast<unsigned>(static_cast<int>(s16(m_.r16(r + 0x58))))) & 0xFFFF) >> 5;
    v = v * m_.r16(r + 0x50);
    out_vol(c, ((v & 0xFFFF) >> 4) & 0x7F);

    // 3. program S: the sample
    {
      const unsigned k = (m_.r8(r + 0x69) - 1) & 0xFF;
      m_.w8(r + 0x69, k);
      int64_t q;
      if (s8(k) < 0 && item_h(r, 0x08, q)) {
        const uint32_t at = m_.r32(q);
        out_lc(c, at & 0xFFFFFF);
        out_len(c, m_.r16(q + 4));
        m_.w8(r + 0x69, (at >> 24) & 0x0F);
        if (!((at & 0x80000000u) && m_.r8(r + 0x53))) m_.w16(r + 0x08, m_.r16(r + 0x08) + 6);
      }
    }

    // 4. program P: the pitch
    if (!m_.r16(r + 0x0A)) {
      m_.w16(r + 0x60, 0);
    } else {
      const unsigned k = (m_.r8(r + 0x66) - 1) & 0xFF;
      m_.w8(r + 0x66, k);
      int64_t q;
      if (s8(k) < 0 && item_h(r, 0x0A, q)) {
        const unsigned item = m_.r16(q);
        m_.w8(r + 0x66, (item >> 8) & 0x0F);
        m_.w16(r + 0x60, item);
        if (!((item & 0x8000) && m_.r8(r + 0x53))) m_.w16(r + 0x0A, m_.r16(r + 0x0A) + 2);
      }
    }

    // 5. the period, and the channel on
    const unsigned flags = m_.r8(r + 0x60);
    int dd = (flags & 0x10) ? 0 : d;
    if (flags & 0x20) dd += s16(m_.r16(r + 0x0E));
    unsigned n = m_.r8(r + 0x61);
    if (!(flags & 0x40)) n = (n + m_.r8(r + 0x02) + m_.r8(r + 0x55)) & 0xFF;
    const unsigned index = n & 0x3F;
    const unsigned base = index < 59 ? kPeriods[36 + index] : index == 59 ? 0 : m_.r16(2 * (index - 60));
    out_per(c, base - m_.r16(r + 0x0E) - static_cast<unsigned>(dd) - m_.r16(r + 0x6A));
    out_dma(0x8000 | (1u << c));
  }

  Memory m_;
  Kind kind_;
  JoPaula* paula_ = nullptr;
  FILE* log_ = nullptr;
  long ticks_ = 0;
  bool flag_[4] = {}, copy_[4] = {}, ended_ = false;
  // L, G
  int64_t voice_[4] = {};
  bool has_[4] = {};
  unsigned dma_off_ = 0, dma_on_ = 0, master_ = 63;
  // H
  int64_t hv_[4] = {};
  unsigned g0_ = 0, g1_ = 0, g2_ = 0, g3_ = 0;
};

// --- telling the songs apart -------------------------------------------------------------

uint16_t word(const std::vector<uint8_t>& f, size_t at) {
  return at + 1 < f.size() ? static_cast<uint16_t>((f[at] << 8) | f[at + 1]) : 0;
}

uint64_t fnv1a(const std::vector<uint8_t>& f) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t b : f) { h ^= b; h *= 0x100000001b3ull; }
  return h;
}

bool is_h(const std::vector<uint8_t>& f) {
  return f.size() >= 8 + 26 && word(f, 0) == 0 && word(f, 4) == 1 && word(f, 6) == 0x7FFF;
}

bool is_lg(const std::vector<uint8_t>& f) {
  const size_t n = word(f, 0);
  if ((n & 1) || n < 4 || n > 0x200 || f.size() < n + 2) return false;
  for (size_t at = 2; at <= n; at += 2) {
    const size_t w = word(f, at);
    if ((w & 1) || w < 2 || w >= 0x8000 || w > f.size() || word(f, w - 2) != 0x7FFF) return false;
  }
  return true;
}

// G or L: the Georg Glaxo songs known by their contents; for others, the tempo bytes of
// the entries with a sequence: G's are $22 (a row every three ticks) or 0 (a row every
// tick), L's $24 and above (G's $22 would be a very slow L tempo).
Kind lg_kind(const std::vector<uint8_t>& f) {
  const uint64_t h = fnv1a(f);
  // FNV-1a 64 of Georg Glaxo's hiscore, ingame and title songs
  if (h == 0xb498b0d60a3b9617ull || h == 0x8a668593ccffb591ull || h == 0x90aff5c80f581c74ull) return kG;
  const size_t lists = word(f, 0) / 2;
  bool any = false;
  for (size_t s = 1; s < lists; ++s) {
    size_t e = word(f, 2 * s);
    for (int n = 0; n < 4096 && e + 10 <= f.size() && word(f, e) != 0x7FFF; ++n, e += 10) {
      if (!word(f, e + 4)) continue;
      if (f[e + 7] >= 0x24) return kL;
      if (f[e + 7] == 0x22) any = true;
    }
  }
  return any ? kG : kL;
}

int subsongs(Kind kind, const std::vector<uint8_t>& f) {
  if (kind == kH) {
    const uint32_t first = (uint32_t(word(f, 18)) << 16) | word(f, 20);
    int n = 0;
    for (size_t rec = 8; rec + 14 <= f.size() && n < 256; rec += 26, ++n)
      if (((uint32_t(word(f, rec + 10)) << 16) | word(f, rec + 12)) != first) break;
    return n;
  }
  const int k = word(f, 0) / 2;
  if (k - 1 == 1) return 1;
  const size_t q = word(f, word(f, 0));
  if (q >= 6 && (word(f, q - 6) == 0 || word(f, q - 4) == 0x7F00)) return k - 2;
  return k - 1;
}

const char* kind_name(Kind k) { return k == kL ? "L" : k == kG ? "G" : "H"; }

// --- the engine ----------------------------------------------------------------------------

class JoEngine : public Engine {
 public:
  bool open(const std::string& path, const std::vector<uint8_t>& file, int track, int rate,
            SongInfo& info, std::string& error) override {
    if (word(file, 0) == 0x6000) { error = "not a song for this engine (it carries its own replay routine)"; return false; }
    if (is_h(file)) kind_ = kH;
    else if (is_lg(file)) kind_ = lg_kind(file);
    else { error = "not a Jesper Olsen song"; return false; }
    const int tracks = subsongs(kind_, file);
    if (tracks < 1) { error = "the song has no subsongs"; return false; }
    if (track >= tracks) { error = "no such track"; return false; }
    info.tracks = tracks;
    info.type = "Jesper Olsen";
    info.comment = kind_ == kL ? "played as in LollyPop" : kind_ == kG ? "played as in Georg Glaxo"
                                                                           : "played as in Harald Hardtand";

    if (const char* name = std::getenv("BBMP_JO_LOG")) {
      if (FILE* f = std::fopen(name, "w")) {
        const size_t slash = path.rfind('/');
        std::fprintf(f, "# jesperolsen paula log 1: song \"%s\" track %d of %d, driver %s\n",
                     (slash == std::string::npos ? path : path.substr(slash + 1)).c_str(), track, tracks,
                     kind_name(kind_));
        Driver d(kind_, file);
        d.log_to(f);
        d.start(track);
        for (int t = 0; t < kMaxTicks && !d.tick(); ++t) {}
        std::fclose(f);
      }
    }

    // the length: the driver alone, without sound
    {
      Driver d(kind_, file);
      d.start(track);
      int t = 0;
      while (t < kMaxTicks && !d.tick()) ++t;
      info.duration_ms = static_cast<long>((static_cast<int64_t>(t + 1) * 70940 * 1000) / 3546895);
      if (t >= kMaxTicks) info.comment += "\nThe song does not end: it is stopped after ten minutes.";
    }

    rate_ = rate;
    driver_.reset(new Driver(kind_, file));
    Memory& m = driver_->memory();
    Audio::Paula::FilterMode filter = Audio::Paula::kFilterModeA500;
    if (const char* f = std::getenv("BBMP_JO_FILTER"))
      filter = !std::strcmp(f, "none") ? Audio::Paula::kFilterModeNone
             : !std::strcmp(f, "a1200") ? Audio::Paula::kFilterModeA1200 : Audio::Paula::kFilterModeA500;
    paula_.reset(new JoPaula(rate, m.d.data(), m.size, static_cast<int64_t>(m.d.size()), filter));
    double panning = 0.4;  // UADE's, which bbmp uses for the other Amiga formats
    if (const char* s = std::getenv("BBMP_JO_PANNING")) panning = std::atof(s);
    paula_->pan(panning);
    driver_->attach(paula_.get());
    driver_->start(track);
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t done = 0;
    while (done < frames) {
      if (pos_ == next_) {
        if (stop_) break;
        if (driver_->tick() || ++ticks_ >= kMaxTicks) stop_ = true;
        next_ = static_cast<int64_t>((static_cast<__int128>(ticks_done_ + 1) * 70940 * rate_) / 3546895);
        ++ticks_done_;
      }
      const size_t want = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(frames - done), next_ - pos_));
      paula_->readBuffer(out + done * 2, static_cast<int>(want * 2));
      done += want;
      pos_ += static_cast<int64_t>(want);
    }
    return done;
  }

 private:
  Kind kind_ = kL;
  std::unique_ptr<Driver> driver_;
  std::unique_ptr<JoPaula> paula_;
  int rate_ = 48000;
  int64_t pos_ = 0, next_ = 0, ticks_done_ = 0;
  long ticks_ = 0;
  bool stop_ = false;
};

Engine* make_jo() { return new JoEngine; }

}  // namespace

REGISTER_ENGINE("jo", make_jo)
