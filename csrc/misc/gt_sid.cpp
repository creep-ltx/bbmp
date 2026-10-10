// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-misc: reSID as GoatTracker's editor drives it.

#include <cstring>
#include <memory>

#include "resid/sid.h"
#include "resid-fp/sidfp.h"

#include "gsid.h"
#include "gt_engine.h"

// in GoatTracker 2's gsid.cpp
extern int clockrate;
extern int samplerate;

namespace {

// For version 1: chips of our own, set up as gsid.cpp sets up its one.
std::unique_ptr<SIDFP> chips[2];

void setup(SIDFP& chip, int rate, bool model8580) {
  chip.set_sampling_parameters(clockrate, SAMPLE_RESAMPLE_INTERPOLATE, rate);
  chip.reset();
  chip.set_chip_model(model8580 ? MOS8580 : MOS6581);
  chip.get_filter().set_distortion_properties(filterparams.distortionrate, filterparams.distortionpoint,
                                              filterparams.distortioncfthreshold);
  chip.get_filter().set_type3_properties(filterparams.type3baseresistance, filterparams.type3offset,
                                         filterparams.type3steepness,
                                         filterparams.type3minimumfetresistance);
  chip.get_filter().set_type4_properties(filterparams.type4k, filterparams.type4b);
  chip.set_voice_nonlinearity(filterparams.voicenonlinearity);
}

}  // namespace

extern "C" void gtw_sid_init(int rate, int model8580, int ntsc) {
  // reSID-fp ("distortion" in the editor) with resampling: the best the editor has
  sid_init(rate, model8580 ? 1 : 0, ntsc ? 1 : 0, 1, 0, 1);
  for (auto& chip : chips) {
    chip.reset(new SIDFP);
    setup(*chip, rate, model8580 != 0);
  }
}

// Version 2: gsid.cpp writes the 25 registers in the order and with the delays of the
// C64 play routine.
extern "C" int gtw_sid_render(const unsigned char* regs, short* mono, int samples) {
  std::memcpy(sidreg, regs, NUMSIDREGS);
  return sid_fillbuffer(mono, samples);
}

// Version 1: its editor wrote filter, ADSR, frequency, pulse and at last the waveforms,
// four cycles apart (sid_fillbuffer in gsid.cpp of GoatTracker 1.53).
extern "C" int gtw_sid_render1(int which, const unsigned char* regs, short* mono, int samples) {
  static const unsigned char groups[4][6] = {
      {0x05, 0x0c, 0x13, 0x06, 0x0d, 0x14},
      {0x00, 0x07, 0x0e, 0x01, 0x08, 0x0f},
      {0x02, 0x09, 0x10, 0x03, 0x0a, 0x11},
      {0x04, 0x0b, 0x12, 0xff, 0xff, 0xff}};
  if (which < 0 || which > 1 || !chips[which]) return 0;
  SIDFP& chip = *chips[which];
  int total = 0;
  auto run = [&](int cycles) {
    while (cycles > 0 && samples > 0) {
      int delta = cycles;
      const int got = chip.clock(delta, mono, samples);   // takes what it ran from delta
      const bool moved = got > 0 || delta < cycles;
      cycles = delta;
      mono += got;
      samples -= got;
      total += got;
      if (!moved) break;
    }
  };
  int budget = static_cast<int>(static_cast<long long>(clockrate) * samples / samplerate);
  for (int r = 0x15; r <= 0x18; r++) chip.write(r, regs[r]);
  for (int g = 0; g < 4; g++) {
    for (int i = 0; i < 6; i++)
      if (groups[g][i] != 0xff) chip.write(groups[g][i], regs[groups[g][i]]);
    if (g < 3) {
      run(4);
      budget -= 4;
    }
  }
  run(budget);
  while (samples > 0) {                                     // what rounding left over
    const int more = static_cast<int>(static_cast<long long>(clockrate) * samples / samplerate);
    if (more <= 0) break;
    const int before = samples;
    run(more);
    if (samples == before) break;
  }
  return total;
}
