// SPDX-License-Identifier: GPL-3.0-or-later
#include "aychip.h"

extern "C" {
#include "ayumi.h"
}

#include <algorithm>
#include <cmath>

namespace ay {

Psg::Psg() {
  ay_[0] = new ayumi;
  ay_[1] = new ayumi;
}

Psg::~Psg() {
  delete ay_[0];
  delete ay_[1];
}

void Psg::setup(int rate, double clock_hz, Chip chip, Layout layout, int chips) {
  chips_ = std::clamp(chips, 1, 2);
  // where the channels A, B and C sit between left (0) and right (1)
  static const double kPan[3][3] = {{0.5, 0.5, 0.5}, {0.2, 0.5, 0.8}, {0.2, 0.8, 0.5}};
  const double* pan = kPan[layout == Layout::ABC ? 1 : layout == Layout::ACB ? 2 : 0];
  for (int c = 0; c < chips_; ++c) {
    if (!ayumi_configure(ay_[c], chip == Chip::YM, clock_hz, rate)) throw Refused("the AY chip can't be set up");
    for (int i = 0; i < 3; ++i) ayumi_set_pan(ay_[c], i, pan[i], 0);
  }
  // three channels at full volume give 1.5 on a side
  gain_ = 32767.0 / 1.5 / chips_;
}

void Psg::write(int chip, const Regs& regs) {
  if (chip < 0 || chip >= chips_) return;
  struct ayumi* a = ay_[chip];
  const uint8_t* r = regs.r;
  for (int i = 0; i < 3; ++i) {
    ayumi_set_tone(a, i, r[i * 2] | ((r[i * 2 + 1] & 0x0F) << 8));
    ayumi_set_mixer(a, i, (r[7] >> i) & 1, (r[7] >> (i + 3)) & 1, (r[8 + i] >> 4) & 1);
    ayumi_set_volume(a, i, r[8 + i] & 0x0F);
  }
  ayumi_set_noise(a, r[6] & 0x1F);
  ayumi_set_envelope(a, r[11] | (r[12] << 8));
  if (regs.shape_written) ayumi_set_envelope_shape(a, r[13] & 0x0F);
}

void Psg::render(int16_t* out, int frames) {
  for (int i = 0; i < frames; ++i) {
    double left = 0, right = 0;
    for (int c = 0; c < chips_; ++c) {
      ayumi_process(ay_[c]);
      ayumi_remove_dc(ay_[c]);
      left += ay_[c]->left;
      right += ay_[c]->right;
    }
    out[i * 2] = static_cast<int16_t>(std::clamp(std::lrint(left * gain_), -32768L, 32767L));
    out[i * 2 + 1] = static_cast<int16_t>(std::clamp(std::lrint(right * gain_), -32768L, 32767L));
  }
}

void FrameEngine::setup(int rate, double frame_hz, double clock_hz, Chip chip, Layout layout, int chips) {
  if (!(frame_hz > 1.0) || !(clock_hz > 1000.0)) throw Refused("no interrupt rate or chip clock");
  psg_.setup(rate, clock_hz, chip, layout, chips);
  per_frame_ = rate / frame_hz;
  owed_ = 0;
  ended_ = false;
}

int FrameEngine::render(int16_t* out, int frames) {
  int done = 0;
  while (done < frames && !ended_) {
    if (owed_ < 1.0) {
      if (!tick()) {
        ended_ = true;
        break;
      }
      owed_ += per_frame_;
    }
    const int n = std::min(frames - done, static_cast<int>(owed_));
    psg_.render(out + done * 2, n);
    done += n;
    owed_ -= n;
  }
  return done;
}

}  // namespace ay
