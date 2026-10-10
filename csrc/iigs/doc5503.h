// SPDX-License-Identifier: BSD-3-Clause
// The Ensoniq 5503 "DOC", the sound chip of the Apple IIGS: 32 oscillators that read
// 8-bit waves from 64 KB of their own RAM.
//
// Written for this player. How the chip behaves (the address arithmetic, what ends
// a wave, swap mode, the one-shot/swap pair that loops) follows R. Belmont's emulation
// in MAME (src/devices/sound/es5503.cpp, BSD-3-Clause, copyright R. Belmont; the licence
// is in csrc/iigs/LICENSE.es5503), which was checked against the real chip. Four things are different here, on purpose:
//   - a wave is as long as its table: an oscillator is at its end when it has gone
//     past the table's last byte, which makes a table of 256 bytes at frequency F
//     repeat F * rate / 2^17 times a second, as Ensoniq's and Apple's manuals have
//     it. MAME ends a wave on reaching its last byte, 0.4 % earlier for 256 bytes;
//   - the oscillators are stepped one output sample at a time, all of them in turn,
//     as the chip does, so that a swap starts the partner on the right sample;
//   - an oscillator that is halted by a write gets its new mode before the halt is
//     carried out, so that "halt, in swap mode" starts the partner. NinjaTracker's
//     looped instruments depend on it and so does NoiseTracker's way of stopping a
//     voice; MAME decides by the mode the oscillator had before;
//   - the oscillator the players use as a clock reads no wave, so that a zero in the
//     first page of the wave RAM can't stop the song.
#ifndef BBMP_DOC5503_H
#define BBMP_DOC5503_H

#include <cstdint>
#include <cstring>

class Doc5503 {
 public:
  // The IIGS feeds the chip 7.15909 MHz; it spends an eighth of that per oscillator
  // and two more turns on refreshing its RAM.
  static constexpr double kClock = 7159090.0;

  struct Listener {
    virtual ~Listener() {}
    virtual void oscillator_ended(int osc) = 0;   // the chip's interrupt
  };

  uint8_t ram[0x10000];

  Doc5503() { reset(); }

  void reset() {
    std::memset(ram, 0x80, sizeof ram);
    std::memset(osc_, 0, sizeof osc_);
    for (Osc& o : osc_) { o.control = 1; o.wtsize = 256; }
    enabled_ = 1;
    pending_ = 0;
  }

  void set_listener(Listener* l) { listener_ = l; }
  void set_clock_oscillator(int osc) { clock_osc_ = osc; }

  double rate() const { return kClock / 8.0 / (enabled_ + 2); }
  // The sides that oscillators have been started on so far: 1 left, 2 right.
  int sides() const { return sides_; }
  int enabled() const { return enabled_; }

  uint8_t read(int reg) const {
    const Osc& o = osc_[reg & 0x1f];
    switch (reg & 0xe0) {
      case 0x00: return o.freq & 0xff;
      case 0x20: return o.freq >> 8;
      case 0x40: return o.vol;
      case 0x60: return o.data;
      case 0x80: return o.pointer;
      case 0xa0: return o.control;
      case 0xc0: return static_cast<uint8_t>((o.size << 3) | o.resolution);
    }
    if (reg == 0xe1) return static_cast<uint8_t>((enabled_ - 1) << 1);
    return 0;
  }

  void write(int reg, uint8_t data) {
    const int n = reg & 0x1f;
    Osc& o = osc_[n];
    switch (reg & 0xe0) {
      case 0x00: o.freq = (o.freq & 0xff00) | data; return;
      case 0x20: o.freq = static_cast<uint16_t>((o.freq & 0x00ff) | (data << 8)); return;
      case 0x40: o.vol = data; return;
      case 0x60: return;
      case 0x80: o.pointer = data; return;
      case 0xa0: {
        const uint8_t old = o.control;
        o.control = data;
        if (!(data & 1) && n != clock_osc_) sides_ |= data & 0x10 ? 1 : 2;
        if ((old & 1) && !(data & 1)) o.acc = 0;                  // started
        else if (!(old & 1) && (data & 1) && (data & 2)) {        // halted while playing
          if (((data >> 1) & 3) == kSwap) start(n ^ 1);
          if ((old & 8) && (data & 8)) raise(n);
        }
        return;
      }
      case 0xc0:
        o.size = (data >> 3) & 7;
        o.resolution = data & 7;
        o.wtsize = static_cast<uint16_t>(256u << o.size);
        return;
    }
    if (reg == 0xe1) enabled_ = ((data >> 1) & 0x1f) + 1;
  }

  // The clock alone, for one sample: all that matters when nobody listens.
  void step_clock() {
    if (clock_osc_ < 0 || clock_osc_ >= enabled_) return;
    Osc& o = osc_[clock_osc_];
    if (o.control & 1) return;
    const int shift = 9 + o.resolution - o.size;
    if ((o.acc >> shift) >= o.wtsize) ended(clock_osc_, false, shift);
    if (!(o.control & 1)) o.acc += o.freq;
    while (pending_) deliver();
  }

  // One sample of the chip's output, as the sum of wave (-128..127) times volume
  // (0..255) per channel. `channels` is 2 on a IIGS with a stereo card: even channel
  // numbers are the right side, odd ones the left (Apple IIGS technical note 19).
  void step(int32_t& left, int32_t& right) {
    left = right = 0;
    for (int n = 0; n < enabled_; ++n) {
      Osc& o = osc_[n];
      if (o.control & 1) continue;
      const int shift = 9 + o.resolution - o.size;
      uint32_t at = o.acc >> shift;
      if (at >= o.wtsize) {
        ended(n, false, shift);
        at = (o.acc >> shift) & (o.wtsize - 1u);
      }
      if (!(o.control & 1)) {
        o.acc += o.freq;
        uint8_t byte = 0x80;
        if (n != clock_osc_) {
          const uint32_t base = (static_cast<uint32_t>(o.pointer) << 8) & ~(o.wtsize - 1u) & 0xffff;
          byte = ram[base + at];
          o.data = byte;
        }
        if (byte == 0) {
          ended(n, true, shift);
        } else {
          const int mode = (o.control >> 1) & 3;
          if (mode != kSync || !(n & 1)) {
            const int32_t v = (static_cast<int>(byte) - 128) * o.vol;
            if (o.control & 0x10) left += v; else right += v;
          } else if (n < 31 && !(osc_[n + 1].control & 1)) {
            osc_[n + 1].vol = byte;     // amplitude modulation of the next oscillator
          }
        }
      }
      while (pending_) deliver();
    }
  }

 private:
  enum { kFree = 0, kOneShot = 1, kSync = 2, kSwap = 3 };

  struct Osc {
    uint32_t acc;
    uint16_t freq, wtsize;
    uint8_t control, vol, data, pointer, size, resolution;
    bool irq;
  };

  void start(int n) {
    osc_[n].control &= ~1;
    osc_[n].acc = 0;
  }

  void raise(int n) {
    if (!osc_[n].irq) { osc_[n].irq = true; ++pending_; }
  }

  void deliver() {
    for (int n = 0; n < 32; ++n) {
      if (!osc_[n].irq) continue;
      osc_[n].irq = false;
      --pending_;
      if (listener_) listener_->oscillator_ended(n);
      return;
    }
    pending_ = 0;
  }

  // The wave came to its end (or to a zero, which ends a wave on this chip).
  void ended(int n, bool zero, int shift) {
    Osc& o = osc_[n];
    int mode = (o.control >> 1) & 3;
    const int partner_mode = (osc_[n ^ 1].control >> 1) & 3;
    if (mode == kSync) {
      if (!(n & 1) && n > 0 && !(osc_[n - 1].control & 1)) osc_[n - 1].acc = 0;
      mode = kFree;
    }
    if (mode != kFree || zero) o.control |= 1;
    else o.acc -= static_cast<uint32_t>(o.wtsize) << shift;       // keeps its phase
    if (mode == kSwap) {
      start(n ^ 1);
    } else if (partner_mode == kSwap && !(n & 1)) {
      // The even oscillator of a pair whose odd one is set to swap starts over.
      // Seen on the real chip; SoundSmith instruments loop this way.
      // (From the top when a zero ended the wave: there is no phase to keep then.)
      o.control &= ~1;
      if (zero) o.acc = 0;
      else o.acc -= static_cast<uint32_t>(o.wtsize) << shift;
    }
    if (o.control & 8) raise(n);
  }

  Osc osc_[32];
  int enabled_ = 1;
  int pending_ = 0;
  int clock_osc_ = -1;
  int sides_ = 0;
  Listener* listener_ = nullptr;
};

#endif
