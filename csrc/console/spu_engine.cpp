// SPDX-License-Identifier: GPL-2.0-or-later
// The "spu" engine: PlayStation music as a dump of the sound chip (SPU) and a log of
// what the game wrote to it, as made by the logging SPU plugins of PlayStation
// emulators. The SPU itself is P.E.Op.S. as found in the Audio Overload SDK.
//
// The file:
//   0x00000  512 KiB of sound RAM. Its first 4 KiB aren't sample memory (the SPU
//            keeps its capture buffers there), so rippers put a header into it:
//            "SPU" and a fourth byte, the game's name at 4, the song's at 0x44 and the
//            maker's at 0x84, 64 bytes each. Many files have no header.
//   0x80000  512 bytes: the registers 0x1F801C00-0x1F801DFF
//   0x80200  the log, in one of two forms:
//     old:   u32 44100, u32 count, then count times { u32 tick, u32 register,
//            u16 value, u16 unused }
//     new:   u32 ticks until the end, u32 tick of the first entry, then entries of
//            { u8 what, ..., u32 tick of the next entry }:
//              0  write  u32 register, u16 value
//              1  read   u32 register
//              2  write by DMA  u32 count of 16-bit words, the words
//              3  read by DMA   u32 count
//              4  XA sound from CD   32 bytes, 16384 bytes
//              5  CD audio   u32 size, the data
//            The last entry of a file is cut short.
//   A tick is one sample at 44100 Hz.

#include "engine.h"
#include "resample.h"

#include <cstdio>
#include <cstring>
#include <memory>

extern "C" {
typedef struct {
  int16_t l, r;
} stereo_sample_t;
int SPUinit(void);
int SPUopen(void);
int SPUclose(void);
void SPUinjectRAMImage(unsigned short* source);
void SPUwriteRegister(unsigned int reg, unsigned short val);
unsigned short SPUreadRegister(unsigned int reg);
int SPUsample(stereo_sample_t* sample);

// what P.E.Op.S. wants from the PlayStation around it
unsigned int psx_ram[4];
void SPUirq(void) {}
void corlett_sample_fade(stereo_sample_t*) {}
}

namespace {

const size_t kRam = 0x80000, kRegs = 0x200, kLog = kRam + kRegs;
const unsigned kBase = 0x1F801C00;
const int kHardwareRate = 44100;
const int kTailMax = 4 * kHardwareRate;      // how long the notes may ring out after the log
const int kTailSilence = kHardwareRate / 4;  // this much silence ends the song earlier

enum {
  R_MainVolL = 0x180, R_MainVolR = 0x182, R_KeyOn1 = 0x188, R_KeyOn2 = 0x18A,
  R_KeyOff1 = 0x18C, R_KeyOff2 = 0x18E, R_EndX1 = 0x19C, R_EndX2 = 0x19E,
  R_Addr = 0x1A6, R_Data = 0x1A8, R_Stat = 0x1AE,
};

inline uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

class SpuEngine : public Engine {
 public:
  ~SpuEngine() override {
    if (opened_) SPUclose();
  }

  bool open(const std::vector<uint8_t>& file, int track, int rate, SongInfo& info, std::string& error) override {
    if (track != 0) { error = "no such track"; return false; }
    if (file.size() < kLog + 8 + 12) { error = "not an SPU log: too small"; return false; }
    data_ = file.data();
    size_ = file.size();
    if (!check(error)) return false;

    if (!std::memcmp(data_, "SPU", 3)) {
      info.comment = text(4);
      info.title = text(0x44);
      info.author = text(0x84);
    }
    info.type = old_ ? "PlayStation SPU log (old)" : "PlayStation SPU log";
    info.tracks = 1;
    info.duration_ms = static_cast<long>((static_cast<uint64_t>(end_tick_ - first_tick_) * 1000) / kHardwareRate);

    SPUinit();
    SPUopen();
    opened_ = true;
    std::vector<unsigned short> ram(kRam / 2);
    for (size_t i = 0; i < kRam / 2; ++i) ram[i] = le16(data_ + i * 2);
    // the header isn't sound
    if (!std::memcmp(data_, "SPU", 3)) std::fill(ram.begin(), ram.begin() + 0x100 / 2, 0);
    SPUinjectRAMImage(ram.data());
    for (unsigned r = 0; r < kRegs; r += 2) {
      // Not the registers that do something when written: a key on would start
      // whatever a voice was last used for, a data write would change the sound RAM.
      if (r == R_KeyOn1 || r == R_KeyOn2 || r == R_KeyOff1 || r == R_KeyOff2 || r == R_Data ||
          r == R_EndX1 || r == R_EndX2 || r == R_Stat)
        continue;
      write(kBase + r, le16(data_ + kRam + r));
    }

    tick_ = first_tick_;
    next_ = first_tick_;
    at_ = kLog + 8;
    event_ = 0;
    if (rate != kHardwareRate) resampler_.reset(new Resampler(kHardwareRate, rate, &SpuEngine::pull, this));
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t n = 0;
    for (; n < frames && !ended(); ++n) {
      if (resampler_) resampler_->frame(out + n * 2);
      else sample(out + n * 2);
    }
    return n;
  }

 private:
  // The whole log is walked through before anything is played: a file that isn't an
  // SPU log doesn't survive that, and the magic can't be asked, as many files lack it.
  bool check(std::string& error) {
    const uint32_t a = le32(data_ + kLog), b = le32(data_ + kLog + 4);
    old_ = a == 44100 && b > 0 && kLog + 8 + static_cast<uint64_t>(b) * 12 <= size_;
    if (old_) {
      events_ = b;
      first_tick_ = 0;
      uint32_t last = 0;
      for (uint32_t i = 0; i < events_; ++i) {
        const uint8_t* e = data_ + kLog + 8 + static_cast<size_t>(i) * 12;
        const uint32_t tick = le32(e), reg = le32(e + 4);
        if (tick < last || reg < kBase || reg >= kBase + 0x400) {
          error = "not an SPU log: entry " + std::to_string(i) + " makes no sense";
          return false;
        }
        last = tick;
      }
      if (kLog + 8 + static_cast<uint64_t>(events_) * 12 + 16 < size_) {
        error = "not an SPU log: something follows the log";
        return false;
      }
      end_tick_ = last;
      return true;
    }
    end_tick_ = a;
    first_tick_ = b;
    if (first_tick_ > end_tick_ || end_tick_ > 4u * 3600 * kHardwareRate) {
      error = "not an SPU log: no sensible length";
      return false;
    }
    size_t at = kLog + 8, count = 0, writes = 0;
    uint32_t tick = first_tick_;
    while (at < size_) {
      size_t next;
      uint32_t reg = kBase;
      if (!entry(at, next, reg)) {
        if (size_ - at > 16) {  // the last entry is cut short, but only that one
          error = "not an SPU log: entry " + std::to_string(count) + " makes no sense";
          return false;
        }
        break;
      }
      const uint32_t then = le32(data_ + next - 4);
      if (then < tick || reg < kBase || reg >= kBase + 0x400) {
        error = "not an SPU log: entry " + std::to_string(count) + " makes no sense";
        return false;
      }
      if (data_[at] == 4 || data_[at] == 5) cd_ = true;
      if (data_[at] == 0 || data_[at] == 2) ++writes;
      tick = then;
      at = next;
      ++count;
    }
    if (!writes) { error = "not an SPU log: nothing is written to the SPU"; return false; }
    return true;
  }

  // The entry at <at>: where the next one starts, and the register it is about.
  // False when it doesn't fit into the file or isn't known.
  bool entry(size_t at, size_t& next, uint32_t& reg) const {
    const size_t left = size_ - at;
    if (left < 5) return false;
    uint64_t body;
    switch (data_[at]) {
      case 0: body = 6; reg = le32(data_ + at + 1); break;
      case 1: body = 4; reg = le32(data_ + at + 1); break;
      case 2: body = 4 + static_cast<uint64_t>(le32(data_ + at + 1)) * 2; break;
      case 3: body = 4; break;
      case 4: body = 32 + 16384; break;
      case 5: body = 4 + static_cast<uint64_t>(le32(data_ + at + 1)); break;
      default: return false;
    }
    if (1 + body + 4 > left) return false;
    next = at + 1 + static_cast<size_t>(body) + 4;
    return true;
  }

  std::string text(size_t at) const {
    const char* p = reinterpret_cast<const char*>(data_ + at);
    std::string s(p, strnlen(p, 64));
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
  }

  void write(uint32_t reg, uint16_t value) {
    const uint32_t r = reg - kBase;
    // P.E.Op.S. has no main volume, and songs fade out with it
    if (r == R_MainVolL || r == R_MainVolR) {
      if (!(value & 0x8000)) main_[r == R_MainVolR] = static_cast<int16_t>(value << 1) >> 1;
      return;
    }
    if (r < kRegs) SPUwriteRegister(reg, value);
  }

  void run_log() {
    if (old_) {
      while (event_ < events_) {
        const uint8_t* e = data_ + kLog + 8 + static_cast<size_t>(event_) * 12;
        if (le32(e) > tick_) break;
        write(le32(e + 4), le16(e + 8));
        ++event_;
      }
      if (event_ >= events_) finish();
      return;
    }
    while (!tail_ && next_ <= tick_) {
      size_t next;
      uint32_t reg = kBase;
      if (tick_ >= end_tick_) { finish(); return; }
      if (at_ >= size_ || !entry(at_, next, reg)) {  // no more entries: on to the end
        next_ = 0xFFFFFFFF;
        break;
      }
      switch (data_[at_]) {
        case 0: write(reg, le16(data_ + at_ + 5)); break;
        case 1: if (reg - kBase < kRegs) SPUreadRegister(reg); break;
        case 2: {
          const uint32_t words = le32(data_ + at_ + 1);
          for (uint32_t i = 0; i < words; ++i) SPUwriteRegister(kBase + R_Data, le16(data_ + at_ + 5 + i * 2));
          break;
        }
        default: break;  // reads by DMA change nothing; sound from CD isn't played
      }
      next_ = le32(data_ + next - 4);
      at_ = next;
    }
    if (!tail_ && tick_ >= end_tick_) finish();
  }

  // The log is over: let go of all keys and let the notes ring out.
  void finish() {
    if (tail_) return;
    tail_ = true;
    SPUwriteRegister(kBase + R_KeyOff1, 0xFFFF);
    SPUwriteRegister(kBase + R_KeyOff2, 0x00FF);
  }

  bool ended() const { return tail_ && (tail_pos_ >= kTailMax || quiet_ >= kTailSilence) && drained_; }

  // one sample of the SPU, at its own rate
  void sample(int16_t* out) {
    if (tail_ && (tail_pos_ >= kTailMax || quiet_ >= kTailSilence)) {
      out[0] = out[1] = 0;
      if (++flush_ >= 64) drained_ = true;  // what is still inside the resampler
      return;
    }
    run_log();
    ++tick_;
    stereo_sample_t s = {0, 0};
    SPUsample(&s);
    int l = s.l * main_[0] / 0x3FFF, r = s.r * main_[1] / 0x3FFF;
    if (tail_) {
      l = static_cast<int>(static_cast<int64_t>(l) * (kTailMax - tail_pos_) / kTailMax);
      r = static_cast<int>(static_cast<int64_t>(r) * (kTailMax - tail_pos_) / kTailMax);
      ++tail_pos_;
      quiet_ = (l > -8 && l < 8 && r > -8 && r < 8) ? quiet_ + 1 : 0;
    }
    out[0] = static_cast<int16_t>(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    out[1] = static_cast<int16_t>(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
  }

  static void pull(void* self, int16_t* frame) { static_cast<SpuEngine*>(self)->sample(frame); }

  const uint8_t* data_ = nullptr;
  size_t size_ = 0, at_ = 0;
  bool old_ = false, opened_ = false, tail_ = false, drained_ = false, cd_ = false;
  uint32_t events_ = 0, event_ = 0, first_tick_ = 0, end_tick_ = 0, tick_ = 0, next_ = 0;
  int main_[2] = {0x3FFF, 0x3FFF};
  int tail_pos_ = 0, quiet_ = 0, flush_ = 0;
  std::unique_ptr<Resampler> resampler_;
};

}  // namespace

Engine* make_spu_engine() { return new SpuEngine; }
