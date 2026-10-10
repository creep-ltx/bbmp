// SPDX-License-Identifier: GPL-3.0-or-later
// SoundSmith songs of the Apple IIGS, with the dialects MegaTracker and NinjaTracker.
//
// Written for this player in a clean room, from a description of the formats only.
// MegaTracker's effect rules follow Ian Schmidt's MegaTracker player as published in MTPng
// (GPL 2 or later), which is why this file is GPL.
//
// A song is a file of 64-row patterns for 14 tracks (note, effect and effect-data areas)
// plus a wave bank beside it, "<song>.W": the DOC's whole 64 KB wave memory and up to 15
// instruments that say which part of it each note range plays. Every track drives a pair of
// oscillators; oscillator 30 is the clock, and every one of its interrupts is a tick, a
// speed's worth of ticks a row.
//
// The dialects share the layout and are told apart by the file: MegaTracker by "IAN" at its
// start, NinjaTracker (songs converted from Amiga modules: 8 tracks, effects close to
// ProTracker's, its own pitch scale and looping) by the fields its converter fills in.

#include "iigs_cr.h"
#include "iigs_cr_tables.h"

#include <dirent.h>

#include <algorithm>
#include <bitset>
#include <cstdlib>

namespace {

using namespace iigscr;

enum Dialect { kSoundSmith, kMegaTracker, kNinjaTracker };

constexpr int kRows = 64;
constexpr int kTracks = 14;
constexpr int kPatternBytes = kRows * kTracks;   // 896
constexpr unsigned kNotes = 600;                 // where the note area starts
constexpr unsigned kOrder = 0x1d8;               // the position list
constexpr unsigned kBankInstruments = 0x10002;   // after the count and the wave memory
constexpr unsigned kBankInstrument = 92;         // bytes per instrument in the bank

inline unsigned freq_of(int note) { return note >= 0 && note < 108 ? kNoteFreq[note] : 0; }

bool same_name(const char* a, const std::string& b) {
  size_t i = 0;
  for (; a[i] && i < b.size(); ++i) {
    unsigned char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return false;
  }
  return !a[i] && i == b.size();
}

// The wave bank: "<song>.W", or else a file in the song's folder named <name>.W, <name>.D,
// <name>.DOC or DOC.DATA in any mix of letter case.
bool find_bank(const std::string& path, Bytes& bank) {
  if (read_file(path + ".W", bank)) return true;
  const size_t slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::string wanted[] = {name + ".W", name + ".D", name + ".DOC", "DOC.DATA"};
  for (const std::string& w : wanted) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    std::string found;
    bool any = false;
    while (dirent* e = readdir(d)) {
      if (same_name(e->d_name, w)) { found = e->d_name; any = true; break; }
    }
    closedir(d);
    if (any && read_file(dir + "/" + found, bank)) return true;
  }
  return false;
}

class SoundSmith : public Song {
 public:
  SoundSmith(const Bytes& song, Bytes&& bank, Dialect dialect)
      : f_(song), bank_(std::move(bank)), dialect_(dialect) {
    count_ = bank_[0];   // the instrument count, 1..15 (checked by the loader)
    blocks_ = word_at(f_, 6);
    for (int i = 0; i < 16; ++i) {
      unsigned s = 0;
      if (dialect_ == kNinjaTracker || i < count_)
        s = word_at(bank_, kBankInstruments + kBankInstrument * (count_ + 1) + 2 * i);
      shift_[i] = std::min(s, 15u);
    }
    for (int i = 0; i < 15; ++i) {
      const unsigned rec = 0x14 + 30 * i;
      volume_[i] = byte_at(f_, rec + 24) / 2;
      loop_[i] = byte_at(f_, rec + 26);
      tune_[i] = kNinjaTune[byte_at(f_, rec + 27) & 15];
    }
    const unsigned stereo = kNotes + 3 * blocks_;
    for (int t = 0; t < kTracks; ++t) {
      const uint64_t at = stereo + 2 * t;
      unsigned side;
      if (dialect_ == kNinjaTracker) side = word_at(f_, at);
      else if (at + 1 < f_.size()) side = word_at(f_, at);
      else side = t & 1 ? 0xffff : 0;
      left_[t] = side != 0;
    }
    type = dialect_ == kMegaTracker   ? "SoundSmith (MegaTracker)"
           : dialect_ == kNinjaTracker ? "SoundSmith (NinjaTracker)"
                                       : "SoundSmith";
    if (dialect_ != kNinjaTracker) {
      for (int i = 0; i < 15; ++i) {
        const unsigned rec = 0x14 + 30 * i;
        const unsigned n = std::min(byte_at(f_, rec), 21u);
        std::string s;
        for (unsigned j = 0; j < n; ++j) {
          unsigned c = byte_at(f_, rec + 1 + j) & 0x7f;
          if (!c) break;
          s += static_cast<char>(c < 0x20 ? ' ' : c);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        names.push_back(s);
      }
      while (!names.empty() && names.back().empty()) names.pop_back();
    }
  }

 protected:
  void begin() override {
    for (int o = 0; o < 32; ++o) wr(0xa0 + o, 0x01);
    std::copy(bank_.begin() + 2, bank_.begin() + 2 + 0x10000, doc_->ram);
    wr(0x1e, dialect_ == kNinjaTracker ? 0xf0 : 0xfa);
    wr(0x3e, 0);
    wr(0x5e, 0);
    wr(0x9e, 0);
    wr(0xde, 0);
    wr(0xe1, 0x3c);
    wr(0xbe, 0x08);
    len_ = byte_at(f_, 0x1d6);
    speed_ = byte_at(f_, 8);
    if (dialect_ == kMegaTracker) speed_ &= 15;
    cell_ = byte_at(f_, kOrder) * kPatternBytes;
    visited_.set(0);
  }

  void interrupt(int osc) override {
    if (osc == kClockOsc) { tick(); return; }
    if (dialect_ == kNinjaTracker) {
      if (osc / 2 >= 8) return;
      const Track& t = tr_[osc / 2];
      wr(0x80 + osc, t.loopPtr);
      wr(0xc0 + osc, t.loopSize);
      wr(0xa0 + osc, t.loopCtl & 0xf7);
      return;
    }
    // An oscillator with its interrupt on that came to its end starts again.
    const int c = rd(0xa0 + osc);
    if (c & 8) wr(0xa0 + osc, c & ~1);
  }

 private:
  struct Track {
    // SoundSmith
    int inst = 0, note = 0, arp = 0, arpNote = 0;
    unsigned sl = 0, slStep = 0, slTo = 0;
    // MegaTracker
    int mi = -1, mn = 60, fx = 0, fd = 0, stopped = 0, arpBase = 0;
    unsigned a0 = 0, a1 = 0, a2 = 0, freq = 0, pTo = 0, pSpeed = 0;
    int up = 0, vSpeed = 0, vDepth = 0, vPhase = 0;
    // NinjaTracker (note and inst shared with SoundSmith)
    int ninjaArp = 0, an[3] = {0, 0, 0};
    int pitch = 0, pStep = 0, pTarget = 0, vol = 0, tune = 0, vUp = 0, vDown = 0;
    int portSpeed = 0, vib = 0, vibKept = 0, vibAdd = 0, vibPos = 0;
    int loopPtr = 0, loopSize = 0, loopCtl = 0;
  };

  unsigned cell(int area, unsigned k) const { return byte_at(f_, kNotes + uint64_t(area) * blocks_ + k); }
  int osc_a(int t) const { return dialect_ == kNinjaTracker ? 2 * t : 2 * t + 2; }

  void pair(int reg, int t, int value) {
    wr(reg + osc_a(t), value);
    wr(reg + osc_a(t) + 1, value);
  }
  void set_freq(int t, unsigned f) {
    pair(0x00, t, f & 0xff);
    pair(0x20, t, (f >> 8) & 0xff);
  }
  // Both oscillators halted with their interrupts off, as a note begins.
  void halt_pair(int t) {
    for (int o = osc_a(t); o <= osc_a(t) + 1; ++o) wr(0xa0 + o, (rd(0xa0 + o) & ~8) | 1);
  }
  bool valid(int i) const { return i >= 0 && i < count_; }
  unsigned shift_of(int i) const { return i >= 0 && i < 16 ? shift_[i] : 0; }

  void end_song() {
    for (int o = 0; o < 30; ++o) wr(0xa0 + o, 0x01);
    ended_ = true;
  }

  void tick() {
    if (ended_) return;
    tick_ = (tick_ + 1) & 0xff;
    const int tracks = dialect_ == kNinjaTracker ? 8 : kTracks;
    if (tick_ != speed_) {
      for (int t = 0; t < tracks; ++t) {
        if (dialect_ == kSoundSmith) ss_between(t);
        else if (dialect_ == kMegaTracker) mt_between(t);
        else nt_between(t);
      }
      return;
    }
    tick_ = 0;
    for (int t = 0; t < tracks; ++t) {
      if (dialect_ == kSoundSmith) ss_row(t, cell_);
      else if (dialect_ == kMegaTracker) mt_row(t, cell_);
      else nt_row(t, cell_);
      ++cell_;
    }
    if (dialect_ == kNinjaTracker) cell_ += kTracks - 8;
    if (++row_ < kRows) return;
    row_ = 0;
    if (dialect_ != kNinjaTracker) {
      if (++pos_ >= len_) { end_song(); return; }
      cell_ = byte_at(f_, kOrder + pos_) * kPatternBytes;
      return;
    }
    pos_ = (pos_ + 1) & 0xffff;
    const unsigned mark = pos_ * 64 + jumpRow_ / 14;
    if (pos_ >= len_ || visited_.test(mark)) { end_song(); return; }
    visited_.set(mark);
    cell_ = byte_at(f_, kOrder + pos_) * kPatternBytes + jumpRow_;
    jumpRow_ = 0;
  }

  // ---- starting a note (SoundSmith and MegaTracker) ----

  void start_note(int t, int inst, int note, int vol) {
    halt_pair(t);
    if (!valid(inst)) return;
    const unsigned first = kBankInstruments + kBankInstrument * inst + 0x20;
    const unsigned end = first + kBankInstrument;
    unsigned ea = first;
    while (byte_at(bank_, ea) < unsigned(note) && ea + 6 < end) ea += 6;
    unsigned eb = ea;
    while (byte_at(bank_, eb) != 0x7f && eb + 6 < end) eb += 6;
    eb += 6;
    while (byte_at(bank_, eb) < unsigned(note) && eb + 6 < end) eb += 6;
    const int side = left_[t] ? 0x10 : 0;
    const int ca = (byte_at(bank_, ea + 3) & 0x0f) | side;
    const int cb = (byte_at(bank_, eb + 3) & 0x0f) | side;
    set_freq(t, freq_of(note) >> shift_of(inst));
    pair(0x40, t, kVolume[vol]);
    const int a = osc_a(t);
    wr(0x80 + a, byte_at(bank_, ea + 1));
    wr(0x81 + a, byte_at(bank_, eb + 1));
    wr(0xc0 + a, byte_at(bank_, ea + 2));
    wr(0xc1 + a, byte_at(bank_, eb + 2));
    wr(0xa0 + a, ca);
    wr(0xa1 + a, cb);
  }

  // ---- SoundSmith ----

  void ss_row(int t, unsigned k) {
    Track& s = tr_[t];
    const int n = cell(0, k);
    if (n >= 0x80) {
      if (n == 0x80) { wr(0xa0 + osc_a(t), 1); wr(0xa1 + osc_a(t), 1); }
      else if (n == 0x81) row_ = 63;
      return;
    }
    const int e1 = cell(1, k), d = cell(2, k);
    if (n) { s.arp = 0; s.note = n; }
    if (e1 >> 4) s.inst = e1 >> 4;
    const int inst = s.inst - 1;
    int v = inst >= 0 && inst < 15 ? volume_[inst] : 0;
    const int fx = e1 & 15;
    if (fx == 0) {
      if (d) { s.arp = d; s.arpNote = s.note; s.sl = 0; }
    } else {
      s.arp = 0;
      switch (fx) {
        case 1: case 2: ss_slide(s, fx, d); break;
        case 3: v = d / 2; break;
        case 5: v = std::min(127, v + d / 2); break;
        case 6: v = std::max(0, v - d / 2); break;
        case 15: speed_ = d & 15; break;
        default: break;
      }
      if ((fx == 3 || fx == 5 || fx == 6) && !n) pair(0x40, t, v);
    }
    if (n) start_note(t, inst, n, v);
  }

  void ss_slide(Track& s, int fx, int d) {
    const int h = d >> 4, sh = d & 15;
    s.sl = freq_of(s.note);
    const int goal = fx == 1 ? h + s.note : std::abs(h - s.note);
    s.note = goal & 0xff;
    s.slTo = freq_of(s.note);
    const bool down = s.slTo < s.sl;
    unsigned step = (down ? s.sl - s.slTo : s.slTo - s.sl) >> sh;
    if (!step) step = 1;
    s.slStep = down ? 65536 - step : step;
  }

  void ss_between(int t) {
    Track& s = tr_[t];
    if (s.arp) {
      const int x = s.arp >> 4, y = s.arp & 15;
      switch (tick_ % 6) {
        case 1: case 4: s.arpNote = (s.arpNote + x) & 0xff; break;
        case 2: case 5: s.arpNote = (s.arpNote + y) & 0xff; break;
        default: s.arpNote = (s.arpNote - x - y) & 0xff; break;
      }
      set_freq(t, freq_of(s.arpNote) >> shift_of(s.inst - 1));
    } else if (s.sl) {
      s.sl = (s.sl + s.slStep) & 0xffff;
      set_freq(t, s.sl);
      if (s.slStep & 0x8000 ? s.sl < s.slTo : s.sl >= s.slTo) s.sl = 0;
    }
  }

  // ---- MegaTracker ----

  void mt_row(int t, unsigned k) {
    Track& s = tr_[t];
    const int n = cell(0, k);
    if (n >= 0x80) {
      if (n == 0x80) { set_freq(t, 0); s.stopped = 1; }
      else if (n == 0x81) row_ = 63;
      return;
    }
    const int e1 = cell(1, k);
    const int h = e1 >> 4;
    if (h) {
      if (s.stopped && s.mi != h - 1) s.stopped = 0;
      s.mi = h - 1;
    }
    const bool ok = valid(s.mi);
    int v = ok ? volume_[s.mi] : 0;
    if (!ok) pair(0x40, t, 0);
    int play = n;
    s.fx = e1 & 15;
    s.fd = cell(2, k);
    const unsigned sh = ok ? shift_[s.mi] : 0;
    const int x = s.fd >> 4, y = s.fd & 15;
    switch (s.fx) {
      case 0:
        if (n) s.arpBase = n;
        if (s.fd) {
          const int q = s.arpBase;
          s.a0 = freq_of(q + x + y) >> sh;
          s.a1 = freq_of(q + x) >> sh;
          s.a2 = freq_of(q + y) >> sh;
        }
        break;
      case 3:
        v = s.fd / 2;
        pair(0x40, t, kVolume[v]);
        break;
      case 4:
        if (x) s.vSpeed = x;
        if (y) s.vDepth = 32 * y;
        break;
      case 5: v = std::min(127, v + s.fd / 2); break;
      case 6: v = std::max(0, v - s.fd / 2); break;
      case 12: {
        const unsigned from = freq_of(s.mn), to = freq_of(n);
        if (s.fd) s.pSpeed = s.fd;
        if (from == to) {
          s.fx = 0;
          s.fd = 0;
        } else {
          s.up = to > from;
          s.pTo = to;
          play = 0;
        }
        break;
      }
      case 15: {
        speed_ = s.fd & 15;
        const unsigned clock = 250 * kMegaTempo[x] / 125;
        wr(0x1e, clock & 0xff);
        wr(0x3e, (clock >> 8) & 0xff);
        break;
      }
      default: break;
    }
    if (!ok || !play) return;
    const bool resume = s.stopped && s.mn == play;
    s.stopped = 0;
    if (resume) {
      set_freq(t, freq_of(play) >> sh);
      if (s.fx != 3) pair(0x40, t, kVolume[v]);
    } else {
      start_note(t, s.mi, play, v);
    }
    s.mn = play;
    s.freq = freq_of(play);
  }

  void mt_between(int t) {
    Track& s = tr_[t];
    const unsigned sh = valid(s.mi) ? shift_[s.mi] : 0;
    switch (s.fx) {
      case 0:
        if (s.fd) {
          const int m = tick_ % 4;
          set_freq(t, m == 0 ? s.a0 : m == 1 ? s.a1 : s.a2);
        }
        break;
      case 1:
        if (s.freq < 65535u - s.fd) {
          s.freq += s.fd;
          set_freq(t, s.freq >> sh);
        }
        break;
      case 2:
        if (s.freq > unsigned(s.fd)) {
          s.freq -= s.fd;
          set_freq(t, s.freq >> sh);
        }
        break;
      case 4: {
        s.vPhase = (s.vPhase + s.vSpeed) % 32;
        const int z = static_cast<int16_t>(s.freq) + kMegaVibrato[(s.vPhase + s.vDepth) % 520];
        set_freq(t, static_cast<uint16_t>(z) >> sh);
        break;
      }
      case 12:
        if (s.up) {
          s.freq = (s.freq + s.pSpeed) & 0xffff;
          if (s.freq >= s.pTo) { s.freq = s.pTo; s.fx = 0; s.fd = 0; }
        } else {
          s.freq = (s.freq - s.pSpeed) & 0xffff;
          if (s.freq <= s.pTo || s.freq > 32768) { s.freq = s.pTo; s.fx = 0; s.fd = 0; }
        }
        set_freq(t, s.freq >> sh);
        break;
      default: break;
    }
  }

  // ---- NinjaTracker ----

  static int note_pos(int m) { return kNinjaNote[std::min(m, 60)]; }
  static unsigned pitch_freq(int p) {
    int i = (p >= 0 ? p / 2 : -((-p + 1) / 2)) + 48;   // rounded toward minus infinity
    i = std::max(0, std::min(815, i));
    return kNinjaPitch[i];
  }
  static int bounded(int p) { return p < 0 ? 0 : p >= 0x600 ? 0x5fe : p; }
  // The two wave entries of instrument i in the bank (0s for one the bank hasn't got).
  unsigned entry(int i, int b) const {
    if (i < 0 || i >= count_) return 0;
    return byte_at(bank_, kBankInstruments + kBankInstrument * i + 0x20 + b);
  }

  void nt_volume(int t, int v) { pair(0x40, t, kNinjaVolume[std::max(0, std::min(129, v))]); }
  static void clear(Track& s) { s.vUp = s.vDown = s.pStep = s.vib = 0; }
  static void slide_to(Track& s, int note, int speed) {
    s.pTarget = note_pos(note) + s.tune;
    s.pStep = s.pTarget >= s.pitch ? speed : -speed;
    s.vUp = s.vDown = s.vib = 0;
  }
  static void volume_slide(Track& s, int d) {
    if (!d) return;
    if (d & 15) { s.vDown = 2 * (d & 15); s.vUp = 0; }
    else { s.vUp = 2 * (d >> 4); s.vDown = 0; }
  }

  void nt_row(int t, unsigned k) {
    Track& s = tr_[t];
    int n = cell(0, k) & 0x7f;
    const int e1 = cell(1, k), d = cell(2, k);
    const int x = d >> 4, y = d & 15;
    if (n) s.note = n;
    const bool named = (e1 >> 4) != 0;
    if (named) s.inst = e1 >> 4;
    const int inst = s.inst - 1;
    const bool known = inst >= 0 && inst < 15;
    int v = known ? volume_[inst] : 0;
    s.tune = known ? tune_[inst] : 0;
    const int fx = e1 & 15;
    if (fx == 0) {
      if (d) {
        s.ninjaArp = 2;
        s.an[0] = s.note;
        s.an[1] = s.note + x < 60 ? s.note + x : 59;
        s.an[2] = s.note + y < 60 ? s.note + y : 59;
      } else {
        s.ninjaArp = 0;
      }
      clear(s);
    } else {
      s.ninjaArp = 0;
      switch (fx) {
        case 1: slide_to(s, 60, 2 * d); break;
        case 2: slide_to(s, 24, 2 * d); break;
        case 3:
          s.portSpeed = (2 * (d ? d : s.portSpeed)) & 0xffff;
          if (n) {
            slide_to(s, n, s.portSpeed);
            n = 0;
          } else {
            if (s.portSpeed) s.pStep = s.pStep < 0 ? -s.portSpeed : s.portSpeed;
            s.vUp = s.vDown = s.vib = 0;
          }
          break;
        case 4:
          if (d) {
            if (y) vibShape_ = y + 1;
            vibSpeed_ = 2 * x;
            if (vibShape_) s.vib = vibShape_;
            s.vibKept = vibShape_;
            if (vibSpeed_) s.vibAdd = vibSpeed_;
            s.vibPos = 0;
          } else {
            s.vib = s.vibKept;
          }
          s.vUp = s.vDown = s.pStep = 0;
          break;
        case 5: s.vib = 0; volume_slide(s, d); break;
        case 6: s.pStep = 0; volume_slide(s, d); break;
        case 10: s.pStep = 0; s.vib = 0; volume_slide(s, d); break;
        case 11: pos_ = (d - 1) & 0xffff; row_ = 63; clear(s); break;
        case 12:
          v = std::min(128, 2 * d);
          if (!n) nt_volume(t, v);
          clear(s);
          break;
        case 13:
          if (d < 64) jumpRow_ = 14 * d;
          row_ = 63;
          clear(s);
          break;
        case 14:
          switch (x) {
            case 1: s.vUp = s.vDown = 0; s.pStep = y; break;
            case 2: s.vUp = s.vDown = 0; s.pStep = -y; break;
            case 10: s.pStep = 0; s.vDown = 0; s.vUp = y; break;
            case 11: s.pStep = 0; s.vUp = 0; s.vDown = y; break;
            default: clear(s); break;
          }
          break;
        case 15:
          if (y) speed_ = y;
          clear(s);
          break;
        default: clear(s); break;   // 7, 8, 9
      }
    }
    if (!n) {
      if (named) { s.vol = v; nt_volume(t, v); }
      return;
    }
    s.vol = v;
    nt_start(t, inst, n, v);
  }

  void nt_start(int t, int inst, int note, int vol) {
    Track& s = tr_[t];
    halt_pair(t);
    if (inst < 0 || inst >= count_ || inst >= 15) return;
    const int side = left_[t] ? 0x10 : 0;
    int ca = (entry(inst, 3) & 0x0f) | side;
    int pb = entry(inst, 7), sb = entry(inst, 8);
    int cb = (entry(inst, 9) & 0x0f) | side;
    const int lk = loop_[inst];
    if (lk == 3) {
      ca = (ca & 0xf0) | 6;
      cb = (cb & 0xf0) | 7;
    } else if (lk) {
      pb = entry(inst + 1, 7);
      sb = entry(inst + 1, 8);
      cb = (entry(inst + 1, 9) & 0xf0) | 7;
      ca |= 8;
    }
    if (lk) cb = (cb & 0x0f) | side;
    s.pitch = note_pos(note) + s.tune;
    set_freq(t, pitch_freq(s.pitch) >> shift_of(inst));
    nt_volume(t, vol);
    const int a = osc_a(t);
    wr(0x80 + a, entry(inst, 1));
    wr(0x81 + a, pb);
    wr(0xc0 + a, entry(inst, 2));
    wr(0xc1 + a, sb);
    wr(0xa0 + a, ca);
    wr(0xa1 + a, cb | 1);
    if (lk) { s.loopPtr = pb; s.loopSize = sb; s.loopCtl = cb; }
  }

  void nt_between(int t) {
    Track& s = tr_[t];
    if (s.ninjaArp) {
      const int w = s.ninjaArp == 4 ? 2 : s.ninjaArp;
      set_freq(t, pitch_freq(bounded(note_pos(s.an[w - 1]) + s.tune)));
      if (++s.ninjaArp == 5) s.ninjaArp = 1;
      return;
    }
    if (s.pStep) {
      s.pitch += s.pStep;
      if ((s.pStep > 0 && s.pitch >= s.pTarget) || (s.pStep < 0 && s.pitch <= s.pTarget)) {
        s.pitch = s.pTarget;
        s.pStep = 0;
      }
      set_freq(t, pitch_freq(s.pitch));
    } else if (s.vib) {
      const int o = kNinjaVibrato[(s.vib - 1) % 16][(s.vibPos / 2) % 32];
      set_freq(t, pitch_freq(bounded(s.pitch + o)));
      s.vibPos += s.vibAdd;
      if (s.vibPos >= 64) s.vibPos = 0;
    }
    int w;
    if (s.vDown) {
      w = s.vol - s.vDown;
      if (w < 0) { w = 0; s.vDown = 0; }
    } else if (s.vUp) {
      w = s.vol + s.vUp;
      if (w > 128) { w = 128; s.vUp = 0; }
    } else {
      return;
    }
    s.vol = w;
    nt_volume(t, w);
  }

  const Bytes f_;
  const Bytes bank_;
  const Dialect dialect_;
  int count_ = 0;
  unsigned blocks_ = 0;
  unsigned shift_[16];
  int volume_[15], loop_[15], tune_[15];
  bool left_[kTracks];

  Track tr_[kTracks];
  int tick_ = 0, speed_ = 0, row_ = 0;
  unsigned pos_ = 0, len_ = 0, cell_ = 0;
  // NinjaTracker
  unsigned jumpRow_ = 0;
  std::bitset<128 * 64> visited_;
  int vibShape_ = 0, vibSpeed_ = 0;
};

}  // namespace

Song* load_soundsmith(const std::string& path, const Bytes& f, std::string& why) {
  const size_t len = f.size();
  const bool ok = len > 6 && f[4] == 'O' && f[5] == 'K';
  const bool ian = len > 6 && f[0] == 'I' && f[1] == 'A' && f[2] == 'N';
  if (!ok && !ian) { why = "not a SoundSmith song"; return nullptr; }
  if (len < 603) { why = "the song is cut off in its header"; return nullptr; }
  const unsigned blocks = word_at(f, 6), positions = f[0x1d6];
  if (!blocks || !positions || positions > 128) { why = "not a SoundSmith song: no patterns"; return nullptr; }
  if (len < kNotes + 2 * uint64_t(blocks)) { why = "the song is cut off in its patterns"; return nullptr; }
  const size_t slash = path.rfind('/');
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  Bytes bank;
  if (!find_bank(path, bank)) { why = "its wave bank is missing: " + name + ".W"; return nullptr; }
  const unsigned n = word_at(bank, 0);
  if (bank.size() < kBankInstruments || !n || n > 15 || bank.size() < kBankInstruments + kBankInstrument * n) {
    why = "the wave bank isn't a SoundSmith wave bank";
    return nullptr;
  }
  Dialect d = kSoundSmith;
  if (ian) {
    d = kMegaTracker;
  } else if (f[0x1d7]) {
    bool ninja = true;
    for (int i = 0; i < 15 && ninja; ++i) {
      const unsigned rec = 0x14 + 30 * i;
      if (word_at(f, rec + 22) || word_at(f, rec + 28) || byte_at(f, rec + 26) > 3) ninja = false;
    }
    if (ninja) d = kNinjaTracker;
  }
  return new SoundSmith(f, std::move(bank), d);
}
