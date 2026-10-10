// SPDX-License-Identifier: GPL-3.0-or-later
// The AY-3-8910 / YM2149 for the engines that make register values once per
// interrupt: Ayumi (by Peter Sovietov) does the sound, this keeps the time.
#ifndef BBMP_AY_AYCHIP_H
#define BBMP_AY_AYCHIP_H

#include "engine.h"

#include <cstdint>

struct ayumi;

namespace ay {

enum class Chip { AY, YM };
enum class Layout { Mono, ABC, ACB };

// Register values of one interrupt. r[13] is only written to the chip when
// shape_written is set, as writing it restarts the envelope.
struct Regs {
  uint8_t r[14] = {0, 0, 0, 0, 0, 0, 0, 0x3F, 0, 0, 0, 0, 0, 0};
  bool shape_written = false;
};

class Psg {
 public:
  Psg();
  ~Psg();
  // chips: 1, or 2 for TurboSound
  void setup(int rate, double clock_hz, Chip chip, Layout layout, int chips = 1);
  void write(int chip, const Regs& regs);
  void render(int16_t* out, int frames);

 private:
  struct ayumi* ay_[2];
  int chips_ = 1;
  double gain_ = 0;
};

// An engine whose song is a row of interrupts. The engine calls setup() in open(),
// and tick() is asked for the registers of every interrupt.
class FrameEngine : public Engine {
 public:
  int render(int16_t* out, int frames) override;

 protected:
  // frame_hz: interrupts per second (48.828 on a Pentagon, 50 on most others)
  void setup(int rate, double frame_hz, double clock_hz, Chip chip, Layout layout, int chips = 1);
  // Makes the registers of the next interrupt and writes them with psg().write().
  // Returns false when the song has ended (nothing is played for that call).
  virtual bool tick() = 0;
  Psg& psg() { return psg_; }

 private:
  Psg psg_;
  double per_frame_ = 0;   // samples per interrupt
  double owed_ = 0;        // samples of the current interrupt not yet rendered
  bool ended_ = false;
};

}  // namespace ay

#endif
