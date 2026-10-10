// SPDX-License-Identifier: GPL-3.0-or-later
// What bbmp-iigs.cpp and the two Apple IIGS engines share: the song interface, file
// reading, and the register log of the test mode.
//
// The engines (ss_cr.cpp: SoundSmith with its dialects MegaTracker and NinjaTracker;
// ntgs_cr.cpp: Noise Tracker GS) were written for this player in a clean room, from a
// description of the formats only. They are GPL because MegaTracker's effect rules follow
// Ian Schmidt's MegaTracker player as published in MTPng (GPL 2 or later).
#ifndef BBMP_IIGS_CR_H
#define BBMP_IIGS_CR_H

#include "doc5503.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

// Reads a whole file; false when it can't be read or is empty.
bool read_file(const std::string& path, Bytes& into, size_t limit = size_t(64) << 20);

// Byte and little-endian word of a file; outside the file they read as 0.
inline unsigned byte_at(const Bytes& f, uint64_t at) { return at < f.size() ? f[at] : 0; }
inline unsigned word_at(const Bytes& f, uint64_t at) {
  return byte_at(f, at) | (byte_at(f, at + 1) << 8);
}

// The test mode's log (BBMP_IIGS_LOG): every register write and wave-memory page copy, under
// a line that says what made it: the start (B), a clock tick (T), another oscillator's
// interrupt (I) or the step after a sample (D). I and D lines are written only when
// something follows them.
struct RegLog {
  FILE* f = nullptr;
  long long sample = 0;
  long long ticks = 0;
  char pending[64] = "";

  void begin() { pending[0] = 0; std::fputs("B\n", f); }
  void tick() {
    pending[0] = 0;
    std::fprintf(f, "T %lld %lld\n", ticks++, sample);
  }
  void interrupt(int osc) { std::snprintf(pending, sizeof pending, "I %d %lld\n", osc, sample); }
  void after() { std::snprintf(pending, sizeof pending, "D %lld\n", sample); }
  void write(int reg, int value) {
    flush();
    std::fprintf(f, "%02x %02x\n", reg & 0xff, value & 0xff);
  }
  void page(int to, uint64_t offset) {
    flush();
    std::fprintf(f, "P %02x %llx\n", to & 0xff, static_cast<unsigned long long>(offset));
  }
  void flush() {
    if (pending[0]) { std::fputs(pending, f); pending[0] = 0; }
  }
};

// A song being played on a DOC. start() sets the chip up; from then on the chip's
// interrupts drive the song, and idle() is called after every sample the chip makes.
class Song : public Doc5503::Listener {
 public:
  std::string title, author, type, comment;
  std::vector<std::string> names;
  RegLog* log = nullptr;

  void start(Doc5503& doc) {
    doc_ = &doc;
    doc.set_clock_oscillator(kClockOsc);
    doc.set_listener(this);
    if (log) log->begin();
    begin();
  }
  bool ended() const { return ended_; }
  void idle() {
    if (log) log->after();
    after_sample();
  }
  void oscillator_ended(int osc) override {
    if (log) {
      if (osc == kClockOsc) log->tick();
      else log->interrupt(osc);
    }
    interrupt(osc);
  }

 protected:
  // Oscillator 30 is the clock in every engine: it reads no wave and is never heard.
  static constexpr int kClockOsc = 30;

  virtual void begin() = 0;
  virtual void interrupt(int osc) = 0;
  virtual void after_sample() {}

  void wr(int reg, int value) {
    if (log) log->write(reg, value);
    doc_->write(reg, static_cast<uint8_t>(value));
  }
  int rd(int reg) const { return doc_->read(reg); }

  Doc5503* doc_ = nullptr;
  bool ended_ = false;
};

// The loaders: a song ready to start, or nullptr and the reason in `why`.
Song* load_soundsmith(const std::string& path, const Bytes& file, std::string& why);
Song* load_noisetracker(const std::string& path, const Bytes& file, std::string& why);

#endif
