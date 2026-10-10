// SPDX-License-Identifier: GPL-3.0-or-later
// The effects of a Skale Tracker song: its mixer's sends and the plug-ins on its
// effect buses, for the skale engine. See skale_fx.cpp.
#ifndef BBMP_SYNTH_SKALE_FX_H
#define BBMP_SYNTH_SKALE_FX_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace skale {

// A plug-in on an effect bus, rewritten from its source: stereo in, stereo out,
// at 44100 Hz, the rate Skale plays at.
class Effect {
 public:
  virtual ~Effect() = default;
  virtual void set_parameter(int index, float value) = 0;
  // out = the plug-in's output for `in`; the two may not overlap
  virtual void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int frames) = 0;
};

// The plug-in a bus names, by the file name of its DLL; nullptr for one that isn't
// rewritten here.
std::unique_ptr<Effect> make_effect(const std::string& dll);

constexpr int kSlots = 16;

struct Bus {
  std::string dll;               // the plug-in's file name, as the song has it
  std::string name;              // what the song calls it
  std::unique_ptr<Effect> effect;
  float volume = 0.8f;           // the bus fader
  float pan = 0.5f;              // the bus's balance
  bool bypassed = false;
};

struct Send { int from, to; float level; };  // one bus to a later one

struct Mixer {
  std::array<Bus, kSlots> buses;
  // gain of each instrument (by the song's instrument number) into each bus
  std::vector<std::array<float, kSlots>> instrument_sends;
  std::vector<Send> bus_sends;
  std::vector<std::string> missing;  // plug-ins the song uses that aren't here
  // buses that get something from instruments and have a plug-in to play
  bool fed(int slot) const;
  bool any() const;
};

// Reads the mixer of an .skm file. False when the file has no effects to play.
bool read_mixer(const uint8_t* data, size_t size, Mixer& mixer);

// The return of the buses for one block: `inputs[s]` holds what the instruments
// send to bus s (stereo interleaved, `frames` frames, or empty); the returns are
// added to `out` (stereo interleaved).
void run_buses(Mixer& mixer, std::array<std::vector<float>, kSlots>& inputs, float* out, int frames);

}  // namespace skale

#endif
