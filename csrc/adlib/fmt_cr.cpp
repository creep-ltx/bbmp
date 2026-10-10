// SPDX-License-Identifier: GPL-3.0-or-later
// FM Tracker (.fmt): Sagitta Software's tracker for the Ad Lib (Morten Stenshorne,
// 1993-1999), as its own player, FM Tracker Player 1.26, plays the songs. Written for this
// player in a clean room, from a description of the format only.
//
// A song is an order list of 64-row patterns with 9 melodic channels, or 6 melodic channels
// and 5 drums in the chip's rhythm mode. Cells are packed: a note with its octave, an effect
// and its argument, or a run of empty cells. The effects are the tracker's own: slides,
// vibrato, volume, instrument changes, jumps and breaks, retrigger, note delay, a fade of the
// whole song, speed and clock changes.
//
// The player runs on a timer of about 291 Hz (the base tick) and does its work on every
// first, second, fourth, eighth or sixteenth of them, by the song's clock. Between it and
// the chip lies a small layer that remembers the frequency registers and the levels. Every
// write is made, also one that would not change its register: the chip emulator takes
// buffered writes one after the other, so the number of writes is part of the sound. The
// test mode (BBMP_ADLIB_LOG, common.h) lists them all.

#include <cstring>

#include "common.h"

namespace adlib {

namespace {

constexpr double kFmtHz = 1193182.0 / 4096;
constexpr uint8_t kId[14] = {0x46, 0x4d, 0x54, 0x52, 0x4b, 0x1a, 0x20, 0x1e, 0x20, 0xa1, 0x20, 0xe1, 0x20, 0x5e};
constexpr char kOldId[] = "Sagitta FM Tracker File Copyright (C) 1993";
// F-numbers of the notes (by note number), then the steps of the fine tune
constexpr int kFreq[26] = {0, 343, 363, 385, 408, 432, 458, 485, 514, 544, 577, 611, 647,
                           0, 19,  20,  22,  23,  24,  26,  27,  29,  30,  33,  34,  36};
constexpr int kKindSize[6] = {12, 10, 5, 5, 5, 5};   // melodic, bass drum, snare, tom, cymbal, hihat

int freqOf(int n) { return n >= 0 && n < 26 ? kFreq[n] : 0; }
int stepOf(int n) { return n >= 0 && n < 13 ? kFreq[n + 13] : 0; }

int normalClock(int c) {
  if (c == 3 || c == 5) return 4;
  if (c >= 6 && c <= 11) return 8;
  if (c >= 12 && c <= 16) return 16;
  return c;
}

// The file, taken apart (and checked: a damaged one leaves `ok` false).
struct Layout {
  bool ok = false;
  int version = 0, patterns = 0;
  std::vector<std::vector<uint8_t>> kinds[6];   // instruments by kind, each a record
  std::vector<std::vector<uint8_t>> pattern;
  std::string comment;
  bool hasComment = false;
};

Layout parse(const std::vector<uint8_t>& d) {
  Layout l;
  const size_t size = d.size();
  if (size < 0xe0) return l;
  const bool oldId = std::memcmp(d.data(), kOldId, 42) == 0;
  if (!oldId && std::memcmp(d.data(), kId, 14) != 0) return l;
  l.version = d[0x49];
  if (l.version < 4 || l.version > 8) return l;
  size_t at = 0xe0;
  for (int k = 0; k < 6; k++) {
    const bool shortHihat = k == 5 && l.version == 4;
    const size_t inFile = shortHihat ? 4 : kKindSize[k];
    for (int i = 0; i < d[0x42 + k]; i++) {
      if (at + inFile > size) return l;
      std::vector<uint8_t> r(kKindSize[k], 0);
      std::memcpy(&r[shortHihat ? 1 : 0], &d[at], inFile);
      l.kinds[k].push_back(r);
      at += inFile;
    }
  }
  l.patterns = d[0x40];
  for (int p = 0; p < l.patterns; p++) {
    if (at + 2 > size) return l;
    const size_t len = d[at] | d[at + 1] << 8;
    if (at + 2 + len > size) return l;
    l.pattern.emplace_back(d.begin() + at + 2, d.begin() + at + 2 + len);
    at += 2 + len;
  }
  if ((d[0x4d] & 1) && at + 2 <= size) {
    const size_t len = d[at] | d[at + 1] << 8;
    const size_t n = size - (at + 2) < len ? size - (at + 2) : len;
    l.comment.assign(reinterpret_cast<const char*>(&d[at + 2]), n);
    l.hasComment = true;
  }
  l.ok = true;
  return l;
}

class Fmt : public Song {
 public:
  Fmt(const std::vector<uint8_t>& d, const Layout& l, Copl& opl, OplLog* log) : d_(d), l_(l), opl_(opl), log_(log) {
    opl.init();
    rhythmByte_ = d[0x48];
    rhythm_ = rhythmByte_ != 0;
    nch_ = rhythm_ ? 11 : 9;
    speed_ = d[0x41];
    slideEveryTick_ = d[0x4b] & 1;
    fastTempo_ = d[0x4e] & 1;
    clock_ = d[0x4c];
    if (l.version < 6 || clock_ == 0 || clock_ > 16) clock_ = 1;
    clock_ = normalClock(clock_);

    reset();
    current_ = static_cast<int8_t>(d[0x60]);
    rhythmMode(rhythmByte_);
    depth(0);
    for (int c = 0; c < nch_; c++) ch_[c].instrument = (d[0x4a] & 1) ? d[0x30 + c] : 0;
    for (int c = 0; c < nch_; c++) setInstrument(c, ch_[c].instrument);
    reading_ = current_;
    pos_ = 0;
    divider_ = 16 / clock_;
  }

  double hz() const override { return kFmtHz; }
  int clock() const { return clock_; }

  bool tick() override {
    if (log_) log_->tickStart();
    const bool goesOn = baseTick();
    if (log_) log_->tickEnd(goesOn);
    return goesOn;
  }

 private:
  struct Channel {
    // the chip layer's shadows
    int b0 = 4, a0 = 0, keyFlag = 1, modLevel = 0, carLevel = 0;
    // the player's
    int effect = 0, argument = 0, position = 0, state = 0, counter = 0, volume = 64, instrument = 0;
    int function8 = 0, frozen = 0, stop = 0;
  };

  void out(int reg, int val) {
    opl_.write(reg & 0xff, val & 0xff);
    if (log_) log_->write(reg, val);
  }

  // ---- the chip layer ----

  bool pair(int c) const { return rhythmBit_ == 0 || c < 7; }
  void pairWrite(int c, int base, int m, int k) {
    if (pair(c)) {
      const int o = 8 * (c / 3) + c % 3;
      out(base + o, m);
      out(base + o + 3, k);
    } else if (c <= 10) {
      static const int kOne[4] = {0x14, 0x12, 0x15, 0x11};   // snare, tom, cymbal, hihat
      out(base + kOne[c - 7], m);
    }
  }
  void setLevel(int c, int m, int k) {
    if (pair(c)) {
      pairWrite(c, 0x40, m, k);
      ch_[c].modLevel = m;
      ch_[c].carLevel = k;
    } else if (c <= 10) {
      pairWrite(c, 0x40, m, k);
      ch_[c].modLevel = m;
    }
  }
  void fbc(int c, int v) {
    if (rhythmBit_ == 0 || c < 6) out(0xc0 + c, v);
  }
  void a0(int c, int v) { out(0xa0 + c, v); }
  void b0(int c, int v) {
    if (rhythmBit_ == 0 || c <= 6) out(0xb0 + c, v);
  }
  void note(int c, int octave, int f) {
    if (rhythmBit_ != 0 && c >= 6) return;
    Channel& k = ch_[c];
    const int b = (f >> 8) + 4 * octave + 0x20;
    if (k.keyFlag == 1) b0(c, k.b0 & 0x1f);   // the key off first
    k.b0 = b & 0xffff;
    k.a0 = f & 0xff;
    a0(c, k.a0);
    b0(c, k.b0);
  }
  void keyOff(int c) {
    if (rhythmBit_ != 0 && c >= 6) return;
    ch_[c].b0 &= 0x1f;
    b0(c, ch_[c].b0);
  }
  void volume(int c, int v, bool both) {
    v = v < 0 ? 0 : v > 64 ? 64 : v;
    if (v == 0) {
      pairWrite(c, 0x40, 0xff, 0xff);
      return;
    }
    v -= 1;
    auto scaled = [v](int l) { return (63 - v * ((63 - l) & 63) / 63) | (l & 0xc0); };
    const Channel& k = ch_[c];
    pairWrite(c, 0x40, both ? scaled(k.modLevel) : k.modLevel, scaled(k.carLevel));
  }
  void slideUp(int c, int d) {
    Channel& k = ch_[c];
    int f = (k.b0 & 3) * 256 + k.a0 + d;
    if (f > 0x2ae) {   // up into the next block
      const int r = f - 0x2ae;
      k.a0 = 0x57;
      k.b0 = (k.b0 & 0xfc) + 5;
      f = (k.b0 & 3) * 256 + k.a0 + r;
    }
    retune(c, f);
  }
  void slideDown(int c, int d) {
    Channel& k = ch_[c];
    int f = (k.b0 & 3) * 256 + k.a0 - d;
    if (f < 0x157) {   // down into the block below
      const int r = 0x157 - f;
      k.a0 = 0xae;
      k.b0 = ((k.b0 & 0xfc) - 2) & 0xffff;
      f = (k.b0 & 3) * 256 + k.a0 - r;
    }
    retune(c, f);
  }
  void retune(int c, int f) {
    Channel& k = ch_[c];
    k.a0 = f & 0xff;
    k.b0 = ((k.b0 & 0xfc) | ((f >> 8) & 0xffff)) & 0xffff;
    b0(c, k.b0);
    a0(c, k.a0);
  }
  void drum(int bit) {
    int k = (drumKeys_ & ~bit) | (depth_ + rhythmBit_);
    out(0xbd, k);
    k |= bit;
    out(0xbd, k);
    drumKeys_ = k & 0x1f;
  }
  void rhythmMode(int on) {
    rhythmBit_ = (on & 1) * 32;
    if (on)
      for (int c = 6; c <= 8; c++) {
        out(0xb0 + c, 1);
        out(0xa0 + c, 0);
      }
    out(0xbd, depth_ + (rhythmBit_ & 0x20));
  }
  void depth(int v) {
    depth_ = (v & 3) * 64;
    out(0xbd, depth_ + rhythmBit_);
  }
  void reset() {
    rhythmBit_ = depth_ = drumKeys_ = 0;
    for (int c = 0; c < 11; c++) {
      Channel& k = ch_[c];
      k.b0 = 4;
      k.a0 = 0;
      k.keyFlag = 1;
      k.modLevel = k.carLevel = 0;
    }
    for (int c = 0; c < 11; c++) {
      pairWrite(c, 0x60, 0xff, 0xff);
      pairWrite(c, 0x80, 0x0f, 0x0f);
      pairWrite(c, 0x40, 0xff, 0xff);
    }
    for (int c = 0; c < 9; c++) out(0xb0 + c, 0x1f);
    for (int r = 0x01; r <= 0xdf; r++) out(r, 0);
    out(0x01, 0x20);
    for (int r = 0xe0; r <= 0xff; r++) out(r, 0);
    for (int c = 0; c < 11; c++) ch_[c].b0 = 4;
  }

  // ---- the player ----

  bool melodic(int c) const { return !rhythm_ || c < 6; }
  bool drumChannel(int c) const { return rhythm_ && c >= 6; }

  // An instrument's byte; numbers outside the kind's instruments read as zeros.
  int ins(int kind, int n, int i) const {
    const auto& v = l_.kinds[kind];
    return n >= 0 && n < static_cast<int>(v.size()) ? v[n][i] : 0;
  }
  int kindOf(int c) const { return drumChannel(c) ? c - 5 : 0; }

  int tuned(int c, int n) const {
    const int t = static_cast<int8_t>(ins(0, ch_[c].instrument, 11));
    int f = freqOf(n);
    if (t != 0) f += static_cast<int16_t>(t * stepOf(n)) / 19;
    return f;
  }

  void setVolume(int c, int j) {
    const int v = ch_[c].volume * global_ / 64 * master_ / 64;
    volume(c, v, drumChannel(c) || (ins(0, j, 8) & 1));
  }

  void setInstrument(int c, int n) {
    const int kind = kindOf(c);
    auto b = [&](int i) { return static_cast<int>(static_cast<int8_t>(ins(kind, n, i))); };
    if (kind == 1) {
      pairWrite(c, 0x20, b(0), b(1));
      pairWrite(c, 0x60, b(4), b(5));
      pairWrite(c, 0x80, b(6), b(7));
      pairWrite(c, 0xe0, b(8), b(9));
      setLevel(c, b(2), b(3));
    } else if (kind > 1) {
      pairWrite(c, 0x20, b(0), 0);
      pairWrite(c, 0x60, b(2), 0);
      pairWrite(c, 0x80, b(3), 0);
      pairWrite(c, 0xe0, b(4), 0);
      setLevel(c, b(1), 0);
    } else {
      pairWrite(c, 0x20, b(0), b(1));
      pairWrite(c, 0x60, b(4), b(5));
      pairWrite(c, 0x80, b(6), b(7));
      fbc(c, b(8));
      pairWrite(c, 0xe0, b(9), b(10));
      setLevel(c, b(2), b(3));
    }
    ch_[c].instrument = n & 0xff;
  }

  void unfreeze(int c) {
    const int kind = kindOf(c), n = ch_[c].instrument;
    if (kind > 1) {
      pairWrite(c, 0x60, ins(kind, n, 2), 0);
      pairWrite(c, 0x80, ins(kind, n, 3), 0);
    } else {
      pairWrite(c, 0x60, ins(kind, n, 4), ins(kind, n, 5));
      pairWrite(c, 0x80, ins(kind, n, 6), ins(kind, n, 7));
    }
    ch_[c].frozen = 0;
  }
  void freeze(int c) {
    pairWrite(c, 0x60, 0, 0);
    pairWrite(c, 0x80, 0, 0);
    ch_[c].frozen = 1;
  }

  void trigger(int c) {
    if (melodic(c)) {
      note(c, ch_[c].argument, tuned(c, ch_[c].position));
      ch_[c].keyFlag = 1;
    } else {
      drum(1 << (10 - c));
    }
  }

  // A step of the fade; says whether it was running.
  bool fadeStep() {
    if (fadeCount_ == -1) return false;
    fadeCount_++;
    const int every = fadeSpeed_ <= 11 ? 12 - fadeSpeed_ : 1;
    const int by = fadeSpeed_ <= 11 ? 1 : fadeSpeed_ - 10;
    if (fadeCount_ >= every) {
      fadeCount_ = 0;
      if (fadeTarget_ < global_) {
        global_ -= by;
        if (fadeTarget_ >= global_) {
          global_ = fadeTarget_;
          fadeCount_ = -1;
        }
      } else {
        global_ += by;
        if (fadeTarget_ <= global_) {
          global_ = fadeTarget_;
          fadeCount_ = -1;
        }
      }
    }
    return true;
  }

  bool baseTick() {
    fadeCounter_++;
    dividerCounter_++;
    if (dividerCounter_ >= divider_) {
      dividerCounter_ = 0;
      if (!playerTick()) return false;
    }
    if (fadeCounter_ >= 16) {
      fadeCounter_ = 0;
      if (fadeStep())
        for (int c = 0; c < nch_; c++) setVolume(c, ch_[c].instrument);
    }
    return true;
  }

  void vibrato(int c) {
    Channel& k = ch_[c];
    const int g = k.argument, h = g >> 4, l = g & 15;
    if (k.state < 2) {
      q_ = h != 0 ? l / h : 0;
      if (q_ != 0) {
        if (k.state == 0) {
          k.position += q_;
          slideUp(c, q_);
          if (k.position >= g) k.state = 1;
        } else if (k.state == 1) {
          k.position -= q_;
          slideDown(c, q_);
          if (k.position <= -l) k.state = 0;
        }
      } else {
        k.state += 2;
      }
    }
    if (k.state > 1) {
      if (q_ > 0) {
        q_--;
        return;
      }
      q_ = l != 0 ? h / l : 0;
      if (k.state == 2 && q_ != 0) {
        k.position += 1;
        slideUp(c, 1);
        if (k.position >= g) k.state = 3;
      } else if (k.state == 3 && q_ != 0) {
        k.position -= 1;
        slideDown(c, 1);
        if (k.position <= -l) k.state = 2;
      }
      q_--;
    }
  }

  bool playerTick() {
    for (int c = 0; c < nch_; c++) {
      Channel& k = ch_[c];
      lastInstrument_ = k.instrument;
      switch (k.effect) {
        case 1:
          if (slideEveryTick_ || rowTick_ == 0) slideUp(c, k.argument);
          break;
        case 2:
          if (slideEveryTick_ || rowTick_ == 0) slideDown(c, k.argument);
          break;
        case 4: vibrato(c); break;
        case 5:
          k.volume -= k.argument & 15;
          k.volume += k.argument >> 4;
          k.volume = k.volume < 0 ? 0 : k.volume > 64 ? 64 : k.volume;
          setVolume(c, lastInstrument_);
          break;
        case 6:
          if (--k.counter > 0) break;
          k.counter = k.state;
          trigger(c);
          break;
        case 7:
          if (--k.counter > 0) break;
          k.effect = 0;
          trigger(c);
          break;
        default: break;
      }
      if (k.stop == 1) {
        unfreeze(c);
        keyOff(c);
      }
      if (k.stop != 0) k.stop--;
    }

    if (rowTick_ >= speed_ && delay_ != 0) {
      delay_--;
      rowTick_ = 0;
    }
    if (rowTick_ < speed_) {
      rowTick_++;
      return true;
    }
    rowTick_ = 0;
    if (brk_ == 0 && started_) row_++;
    started_ = true;
    if (brk_ != 0 || row_ > 63) {
      if (jump_ < 0x80) {
        const int before = order_;
        order_ = jump_;
        if (order_ <= before) return false;   // it went back: the song ends
      } else {
        order_++;
      }
      jump_ = 0x80;
      current_ = order_ < 128 ? static_cast<int8_t>(d_[0x60 + order_]) : -1;
      brk_ = 0;
      if (row_ > 63) row_ = 0;
      if (current_ < 0) return false;   // the end of the order list
      skip_ = 0;
      reading_ = current_;
      pos_ = 0;
      if (row_ != 0) seek(row_);
    }
    for (int c = 0; c < nch_; c++) cell(c);
    return true;
  }

  uint8_t pat(int at) const {
    if (reading_ < 0 || reading_ >= static_cast<int>(l_.pattern.size())) return 0;
    const auto& p = l_.pattern[reading_];
    return at >= 0 && at < static_cast<int>(p.size()) ? p[at] : 0;
  }

  void seek(int r) {
    int n = 0, at = 0, last = 0;
    while (n < r * nch_) {
      const int b = pat(at);
      last = b >> 4;
      if (last == 15) {
        n += (b & 15) + 1;
        at += 1;
      } else {
        at += (!rhythm_ || n % 11 < 6) ? 3 : 2;
        n += 1;
      }
    }
    pos_ = at;
    skip_ = last == 15 ? n - r * nch_ + 1 : 0;
    row_ = r;
    started_ = false;
  }

  void cell(int c) {
    Channel& k = ch_[c];
    k.effect = 0;
    if (skip_ != 0) {
      skip_--;
      if (skip_ != 0) return;
    }
    const bool mel = melodic(c);
    const int b0v = pat(pos_);
    const int n = b0v >> 4;
    int octave, e, a;
    if (mel) {
      octave = b0v & 15;
      e = pat(pos_ + 1) & 15;
      a = pat(pos_ + 2);
    } else {
      octave = e = b0v & 15;
      a = pat(pos_ + 1);
    }
    if (n == 15) {   // a run of empty cells
      skip_ = octave + 1;
      pos_ += 1;
      return;
    }
    int vol = 65;   // none
    bool legato = false;
    q_ = 17;
    const int x = a & 15;
    switch (e) {
      case 3:
        k.keyFlag = 0;
        legato = true;
        break;
      case 8:
        switch (k.function8) {
          case 0:
            global_ = a;
            for (int j = 0; j < nch_; j++) setVolume(j, ch_[j].instrument);
            break;
          case 1: setLevel(c, a, k.carLevel); break;
          case 2: setLevel(c, k.modLevel, a); break;
          case 3:
            fadeCount_ = 0;
            fadeTarget_ = a < 64 ? a : 64;
            break;
          default: break;
        }
        break;
      case 9:
        setInstrument(c, a);
        setVolume(c, lastInstrument_);
        break;
      case 11:
        if (a < 0x80) jump_ = a;
        break;
      case 12:
        vol = a < 64 ? a : 64;
        if (n != 0) q_ = a / 4;
        if (q_ > 16) q_ = 16;
        break;
      case 13:
        brk_ = 1;
        row_ = a < 0x80 ? a : 0;
        break;
      case 14:
        switch (a >> 4) {
          case 0x5: fadeSpeed_ = x; break;
          case 0x8: depth(x); break;
          case 0x9:
            if (x == 0 && n != 0) break;
            k.effect = 6;
            if (n == 0) {
              if (x != 0) k.counter = x;
            } else {
              k.argument = octave;
              k.position = n;
              k.state = x;
              k.counter = x;
            }
            break;
          case 0xa: setLevel(c, x * 4, k.carLevel); break;
          case 0xb: setLevel(c, k.modLevel, x * 4); break;
          case 0xd:
            if (x != 0 && n != 0) {
              k.effect = 7;
              k.argument = octave;
              k.position = n;
              k.counter = x;
            }
            break;
          case 0xe: delay_ = x; break;
          case 0xf: k.function8 = x; break;
          default: break;
        }
        break;
      case 15:
        if (a < 0x20 || (fastTempo_ && a <= 0x7f)) {
          speed_ = a;
        } else if (a < 0x30) {
          clock_ = normalClock(a - 31);
          divider_ = 16 / clock_;
          fadeCounter_ = dividerCounter_ = 0;
        }
        break;
      default: break;
    }
    lastInstrument_ = k.instrument;
    if (n != 0 && vol > 64) vol = legato ? k.volume : 64;
    if (vol <= 64 && !legato && n >= 1 && n <= 12 && k.effect != 7 && q_ == 17) q_ = 16;
    if (vol <= 64 && n != 13) {
      k.volume = vol;
      setVolume(c, lastInstrument_);
      if (!legato) unfreeze(c);
    }
    if (n >= 1 && n <= 12 && k.effect != 7) {
      if (mel) {
        note(c, octave, tuned(c, n));
        k.keyFlag = 1;
      } else {
        drum(1 << (10 - c));
      }
    }
    if (n == 13 && octave == 0) keyOff(c);
    switch (e) {
      case 1:
      case 2:
        k.effect = e;
        k.argument = a;
        break;
      case 4:
        k.effect = 4;
        if (a & 0xf0) k.argument = 0x100 - (a & 0xf0);
        if (a & 15) k.argument = (k.argument & 0xf0) | (a & 15);
        k.state &= 1;
        break;
      case 10:
        k.effect = 5;
        k.argument = a;
        break;
      case 14:
        if ((a >> 4) == 0) {
          if (x == 0) {
            unfreeze(c);
            keyOff(c);
          } else {
            k.stop = x;
          }
        } else if ((a >> 4) == 1) {
          if (x == 0) unfreeze(c);
          else if (x == 1) freeze(c);
        }
        break;
      default: break;
    }
    pos_ += mel ? 3 : 2;
  }

  const std::vector<uint8_t>& d_;
  const Layout& l_;
  Copl& opl_;
  OplLog* log_;
  Channel ch_[11];
  // the chip layer
  int rhythmBit_ = 0, depth_ = 0, drumKeys_ = 0;
  // the player
  int rhythmByte_ = 0, nch_ = 9;
  bool rhythm_ = false, slideEveryTick_ = false, fastTempo_ = false;
  int order_ = 0, current_ = 0, row_ = 0, rowTick_ = 0, brk_ = 0, jump_ = 0x80, delay_ = 0;
  bool started_ = false;
  int reading_ = 0, pos_ = 0, skip_ = 0;
  int speed_ = 0, clock_ = 1, divider_ = 16, global_ = 64, master_ = 64;
  int fadeCount_ = -1, fadeTarget_ = 0, fadeSpeed_ = 8;
  int fadeCounter_ = 0, dividerCounter_ = 0;
  int q_ = 0, lastInstrument_ = 0;
};

}  // namespace

int playFmtCr(const char* path, int track, int rate, long long startMs) {
  (void)track;   // one song per file
  const std::vector<uint8_t> d = readFile(path);
  const Layout l = parse(d);
  if (!l.ok) return fail("not an FM Tracker song (versions 4-8)");
  long long ticks = 0;
  int clock = 1;
  {
    Chip probe(8000);
    Fmt p(d, l, probe, nullptr);
    clock = p.clock();
    const long long most = static_cast<long long>(1200 * kFmtHz);
    while (ticks < most && p.tick()) ticks++;
  }
  Info info;
  if (l.version >= 7) info.title = fromCp437(field(&d[0x10], 32));
  if (info.title.empty()) info.title = baseName(path);
  info.type = "FM Tracker (Sagitta Software)";
  info.facts.emplace_back("format", "FMT version " + std::to_string(l.version));
  info.facts.emplace_back("channels", d[0x48] ? "6 melodic + 5 drums" : "9 melodic");
  info.facts.emplace_back("patterns", std::to_string(l.patterns));
  info.facts.emplace_back("timer", std::to_string(clock) + " x 18.2 Hz, speed " + std::to_string(d[0x41]));
  info.facts.emplace_back("player", "FM Tracker Player 1.26 (FMTP.EXE); played by a replayer written for this player");
  if (l.hasComment) info.comment = fromCp437(l.comment);
  info.durationMs = static_cast<long long>(ticks * 1000 / kFmtHz);
  OplLog log("fmtracker", path, 0, info, ticks, kFmtHz, rate);
  Chip opl(rate);
  Fmt song(d, l, opl, log.on() ? &log : nullptr);
  return play(song, opl, info, rate, startMs);
}

}  // namespace adlib
