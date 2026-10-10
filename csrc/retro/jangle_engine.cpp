// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro's engine "onyx": Onyx Music Files (.omf), the songs of the music disk
// "Jangle" by Onyx (MS-DOS, 1993).
//
// Written for this player in a clean room, from a description of the format only.
// An Onyx song is a module of 1 to 32 channels in a layout of its own: the sequence and the
// pattern rows stored backwards, unsigned samples, a tune per sample, two loudness bytes.
// The engine turns it into a classic 31-sample module (the layout of ProTracker and its
// multichannel relatives), changing the few effects that the disk's songs use differently,
// and has libopenmpt play that module.
//
// Loudness: the song asks for a gain (its amplification byte over its divisor). How loud
// libopenmpt plays one channel depends on the number of channels, so that is measured once
// with a small module of one sample at full volume, and the master gain set so that the
// song comes out at the gain it asks for. Even channels are panned full left, odd ones
// full right.
//
// libopenmpt dithers its 16-bit output with a random seed, so by default two runs differ
// in the last bit; the gain is measured without dither, so it is the same every time.
// Test mode: with BBMP_ONYX_NODITHER set, dither is off for the song too and the output
// is repeatable;
// with BBMP_ONYX_DUMP=<prefix>, the module made of the song is written to <prefix>.mod and
// the gains to <prefix>.txt.

#include "engine.h"

#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_ext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using retro::le16;

const char kMagic[16] = {'O', 'n', 'y', 'x', ' ', 'M', 'u', 's', 'i', 'c', ' ', 'F', 'i', 'l', 'e', 0x1A};
const size_t kSmallest = 1309;
const size_t kSequence = 18, kSpeeds = 274, kChannels = 402, kPatterns = 403, kLength = 404;
const size_t kTitle = 405, kSamples = 436, kLoudness = 1304, kBlocks = 1306;
const unsigned kPeriodLow = 0x48, kPeriodHigh = 0x37F;   // periods outside are dropped
const double kTuneStep = 487.125;                       // tune per finetune step

struct Sample {
  std::string name;     // the raw bytes, up to the first zero, trailing blanks off
  unsigned volume = 0, length = 0, loop = 0;
  int tune = 0;
  const uint8_t* data = nullptr;
};

void put_be16(std::vector<uint8_t>& m, unsigned v) {
  m.push_back(static_cast<uint8_t>(v >> 8));
  m.push_back(static_cast<uint8_t>(v));
}

std::string channel_mark(int channels) {
  if (channels == 4) return "M.K.";
  char buf[8];
  std::snprintf(buf, sizeof(buf), channels < 10 ? "%dCHN" : "%dCH", channels);
  return buf;
}

// The module's header up to and with its channel mark: a title, 31 samples, the orders.
void module_head(std::vector<uint8_t>& m, const uint8_t* title20, const std::vector<Sample>& samples,
                 int positions, const std::vector<int>& orders, int channels) {
  m.insert(m.end(), title20, title20 + 20);
  for (const Sample& s : samples) {
    uint8_t name[22] = {0};
    std::memcpy(name, s.name.data(), std::min<size_t>(s.name.size(), 21));
    m.insert(m.end(), name, name + 22);
    put_be16(m, (s.length + 1) / 2);
    long steps = std::lround(-s.tune / kTuneStep);
    steps = std::clamp(steps, -8L, 7L);
    m.push_back(static_cast<uint8_t>(steps & 15));
    m.push_back(static_cast<uint8_t>(s.volume));
    if (s.loop >= 2) {
      put_be16(m, (s.length - s.loop) / 2);
      put_be16(m, s.loop / 2);
    } else {
      put_be16(m, 0);
      put_be16(m, 1);
    }
  }
  m.push_back(static_cast<uint8_t>(positions));
  m.push_back(127);
  for (int i = 0; i < 128; ++i) m.push_back(static_cast<uint8_t>(i < positions ? orders[i] : 0));
  const std::string mark = channel_mark(channels);
  m.insert(m.end(), mark.begin(), mark.end());
}

bool no_dither() { return std::getenv("BBMP_ONYX_NODITHER") != nullptr; }

openmpt_module_ext* create(const std::vector<uint8_t>& m) {
  return openmpt_module_ext_create_from_memory(m.data(), m.size(), openmpt_log_func_silent, nullptr,
                                               nullptr, nullptr, nullptr, nullptr, nullptr);
}

// How loud libopenmpt plays one channel at full volume, with <channels> channels.
double library_gain(int channels) {
  std::vector<Sample> samples(31);
  samples[0].length = 64;
  samples[0].volume = 64;
  samples[0].loop = 64;          // loop start 0, loop length 32 words
  const uint8_t title[20] = {0};
  std::vector<uint8_t> m;
  module_head(m, title, samples, 1, std::vector<int>(128, 0), channels);
  const size_t pattern = m.size();
  m.resize(pattern + 256 * channels, 0);
  m[pattern + 0] = 0x01;
  m[pattern + 1] = 0xAC;
  m[pattern + 2] = 0x10;
  m.insert(m.end(), 32, 0x7F);
  m.insert(m.end(), 32, 0x80);

  openmpt_module_ext* ext = create(m);
  if (!ext) return 0;
  double gain = 0;
  openmpt_module* mod = openmpt_module_ext_get_module(ext);
  openmpt_module_ext_interface_interactive2 i2;
  if (openmpt_module_get_num_channels(mod) == channels &&
      openmpt_module_ext_get_interface(ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE2, &i2, sizeof(i2))) {
    // measured without dither always, so that the gain is the same on every run
    openmpt_module_ctl_set_integer(mod, "dither", 0);
    openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, 100);
    openmpt_module_set_render_param(mod, OPENMPT_MODULE_RENDER_MASTERGAIN_MILLIBEL, 0);
    i2.set_channel_panning(ext, 0, -1.0);
    std::vector<int16_t> pcm(4800 * 2);
    const size_t got = openmpt_module_read_interleaved_stereo(mod, 48000, 4800, pcm.data());
    std::vector<int> left;
    for (size_t f = 1200; f < got; ++f) left.push_back(std::abs(static_cast<int>(pcm[f * 2])));
    if (left.size() > 1000) {
      std::nth_element(left.begin(), left.begin() + left.size() / 2, left.end());
      gain = left[left.size() / 2] / 32768.0 / (127.5 / 128);
    }
  }
  openmpt_module_ext_destroy(ext);
  return gain;
}

class OnyxEngine : public retro::Engine {
 public:
  ~OnyxEngine() override {
    if (ext_) openmpt_module_ext_destroy(ext_);
  }

  bool open(const std::vector<uint8_t>& f, const std::string&, int, int rate, retro::SongInfo& info,
            std::string& error) override {
    const size_t L = f.size();
    if (L < kSmallest || std::memcmp(f.data(), kMagic, 16) != 0) return refuse(error, "not an Onyx Music File");
    if (le16(&f[16]) != 0x0100) return refuse(error, "an Onyx Music File of an unknown version");
    for (int i = 0; i < 128; ++i)
      if (f[kSequence + 2 * i] & 15) return refuse(error, "damaged: the sequence");
    for (int i = 0; i < 128; ++i) {
      const uint8_t s = f[kSpeeds + i];
      if (s & 0x60) return refuse(error, "damaged: the speeds of the positions");
      if (!(s & 0x80)) return refuse(error, "a speed is set for a position, which this player doesn't do");
    }
    const int channels = f[kChannels] + 1, patterns = f[kPatterns] + 1;
    if (channels > 32 || patterns > 64 || (f[kLength] & 1))
      return refuse(error, "damaged: the numbers of channels, patterns or positions");
    const int positions = f[kLength] / 2 + 1;
    info.title = retro::from_cp437(retro::field(&f[kTitle], 31));

    std::vector<Sample> samples(31);
    for (int i = 0; i < 31; ++i) {
      const uint8_t* h = &f[kSamples + 28 * i];
      Sample& s = samples[i];
      s.name = retro::field(h, 21);
      s.volume = h[21];
      s.tune = static_cast<int16_t>(le16(h + 22));
      s.length = le16(h + 24);
      s.loop = le16(h + 26);
      if (s.volume > 64) return refuse(error, "damaged: a sample's volume");
      if (s.loop > s.length) return refuse(error, "damaged: a sample's loop");
      info.names.push_back(retro::from_cp437(s.name));
    }
    while (!info.names.empty() && info.names.back().empty()) info.names.pop_back();

    const size_t block = 3 + 256 * static_cast<size_t>(channels);
    size_t at = kBlocks;
    std::vector<const uint8_t*> cells(patterns);
    for (int p = 0; p < patterns; ++p) {
      if (at + block > L || f[at] != 0 || le16(&f[at + 1]) != 256u * channels)
        return refuse(error, "damaged or of another kind: pattern " + std::to_string(p));
      cells[p] = &f[at + 3];
      at += block;
    }
    for (Sample& s : samples) {
      if (!s.length) continue;
      if (at + 3 + s.length > L || le16(&f[at + 1]) != s.length) return refuse(error, "damaged: the samples");
      s.data = &f[at + 3];
      at += 3 + s.length;
    }

    // the module
    const unsigned unit = 16u * channels;
    std::vector<int> orders(128, 0);
    for (int i = 0; i < positions; ++i) {
      const unsigned w = le16(&f[kSequence + 2 * (positions - 1 - i)]);
      if (w % unit || w / unit >= static_cast<unsigned>(patterns))
        return refuse(error, "damaged: a position asks for a pattern that isn't there");
      orders[i] = static_cast<int>(w / unit);
    }
    std::vector<uint8_t> m;
    module_head(m, &f[kTitle], samples, positions, orders, channels);
    for (int p = 0; p < patterns; ++p) {
      for (int r = 0; r < 64; ++r) {
        for (int c = 0; c < channels; ++c) {
          const uint8_t* cell = cells[p] + (63 - r) * 4 * channels + c * 4;
          unsigned period = cell[0] | (cell[1] & 15) << 8;
          unsigned effect = cell[1] >> 4, sample = cell[2], value = cell[3];
          if (period && (period >= kPeriodHigh || period <= kPeriodLow)) period = effect = sample = value = 0;
          if (sample > 31) sample = 0;
          convert_effect(effect, value);
          m.push_back(static_cast<uint8_t>((sample & 0xF0) | (period >> 8)));
          m.push_back(static_cast<uint8_t>(period));
          m.push_back(static_cast<uint8_t>((sample & 15) << 4 | effect));
          m.push_back(static_cast<uint8_t>(value));
        }
      }
    }
    for (const Sample& s : samples) {
      for (unsigned i = 0; i < s.length; ++i) m.push_back(static_cast<uint8_t>(s.data[i] - 128));
      if (s.length & 1) m.push_back(0);
    }

    int amplification = f[kLoudness];
    if (amplification == 255) amplification = 254;
    const int divisor = ((f[kLoudness + 1] & 0x7F) + 1) / 2;
    if (!amplification || !divisor) return refuse(error, "damaged: the loudness");
    const double song_gain = amplification / 255.0 / divisor;

    // playing it
    ext_ = create(m);
    if (!ext_) return refuse(error, "libopenmpt doesn't take the module made of the song");
    mod_ = openmpt_module_ext_get_module(ext_);
    if (openmpt_module_get_num_channels(mod_) != channels)
      return refuse(error, "libopenmpt sees another number of channels");
    if (no_dither()) openmpt_module_ctl_set_integer(mod_, "dither", 0);
    openmpt_module_set_repeat_count(mod_, 0);
    openmpt_module_set_render_param(mod_, OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT, 100);
    const double lib_gain = library_gain(channels);
    if (lib_gain < 0.01) return refuse(error, "libopenmpt's loudness can't be measured");
    const long millibel = std::lround(2000 * std::log10(song_gain / lib_gain));
    openmpt_module_set_render_param(mod_, OPENMPT_MODULE_RENDER_MASTERGAIN_MILLIBEL, static_cast<int32_t>(millibel));
    if (const char* prefix = std::getenv("BBMP_ONYX_DUMP")) {
      if (FILE* o = std::fopen((std::string(prefix) + ".mod").c_str(), "wb")) {
        std::fwrite(m.data(), 1, m.size(), o);
        std::fclose(o);
      }
      if (FILE* o = std::fopen((std::string(prefix) + ".txt").c_str(), "w")) {
        std::fprintf(o, "channels %d\nmagic %s\nsong_gain %.17g\nlibrary_gain %.17g\nmaster_gain_millibel %ld\n",
                     channels, channel_mark(channels).c_str(), song_gain, lib_gain, millibel);
        std::fclose(o);
      }
    }
    openmpt_module_ext_interface_interactive2 i2;
    if (!openmpt_module_ext_get_interface(ext_, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE2, &i2, sizeof(i2)))
      return refuse(error, "this libopenmpt can't place the channels");
    for (int c = 0; c < channels; ++c) i2.set_channel_panning(ext_, c, c % 2 ? 1.0 : -1.0);

    info.type = "Onyx Music File";
    info.duration_ms = static_cast<long long>(openmpt_module_get_duration_seconds(mod_) * 1000 + 0.5);
    rate_ = rate;
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t done = 0;
    while (done < frames && mod_) {
      const size_t got = openmpt_module_read_interleaved_stereo(mod_, rate_, frames - done, out + done * 2);
      if (!got) break;
      done += got;
    }
    return done;
  }

 private:
  static bool refuse(std::string& error, const std::string& why) {
    error = why;
    return false;
  }

  // The few effects the disk's songs use otherwise than a classic module does.
  static void convert_effect(unsigned& effect, unsigned& value) {
    const bool big = value >= 0x10, bit3 = value & 8;
    switch (effect) {
      case 0x5:
        if (big) {
          if (bit3) { effect = 3; value = 0; }
          else value &= 0xF0;
        }
        break;
      case 0x6:
        if (big) value &= 0xF0;
        break;
      case 0xA:
        if (big) {
          if (bit3) effect = value = 0;
          else value &= 0xF0;
        }
        break;
      case 0x8:
        effect = value = 0;
        break;
      case 0xC:
        if (value > 0x40) effect = value = 0;
        break;
      case 0xD:
        if (value > 0x3F) effect = value = 0;
        else value = (value / 10) * 16 + value % 10;
        break;
      case 0xE: {
        const unsigned hi = value >> 4;
        if (hi == 0 || hi == 3 || hi == 6 || hi == 8 || hi == 0xF) effect = value = 0;
        break;
      }
      case 0xF:
        if (value > 0x1F) effect = value = 0;
        break;
    }
  }

  openmpt_module_ext* ext_ = nullptr;
  openmpt_module* mod_ = nullptr;
  int rate_ = 48000;
};

}  // namespace

REGISTER_ENGINE("onyx", OnyxEngine)
