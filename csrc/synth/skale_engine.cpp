// SPDX-License-Identifier: GPL-3.0-or-later
// Skale Tracker (.skm) through libopenmpt with Luke Preiner's experimental SKM
// loader built into it (github.com/lpreiner/skale-skm-loader-openmpt). Skale is
// closed; its own player exists only as a Windows library. The loader was written
// from the files and checked against Skale's own recordings.
//
// The effects of a song (delay, reverb and a few others, VST plug-ins in Skale)
// are played here, see skale_fx.cpp: the song is loaded once as it is, for the dry
// mix, and once more for each effect bus the instruments send to, with each
// instrument's gain set to its send (the loader has a hook for that, added by
// tools/build-synth.sh). A song with VST instruments is refused here, because its
// notes would be missing. Without effects the song plays at 48 kHz; with them at
// 44.1 kHz, Skale's rate, which the plug-ins' delays are counted in.

#include "engine.h"
#include "skale_fx.h"

#include <libopenmpt/libopenmpt.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// in the loader, see tools/build-synth.sh
extern "C" int modland_skm_hook;
extern "C" float modland_skm_gain[256];

namespace {

openmpt_module* g_module;
bool g_ended;
int g_rate = 48000;
skale::Mixer g_mixer;
openmpt_module* g_sends[skale::kSlots];   // what the instruments send to each bus
std::array<std::vector<float>, skale::kSlots> g_inputs;

void log_nothing(const char*, void*) {}

uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// An .skm file is a row of chunks: id, tag, length, then so many bytes. The first
// has tag 0xFFFE and says "ALIM3"; tags 1 to 6 are the sections; 0xFFFF ends the
// row. Section 6 holds the VST instruments, as a row of chunks of its own.
bool is_skm(const uint8_t* d, size_t size) {
  return size >= 17 && u16(d + 2) == 0xFFFE && u32(d + 4) == 9 && std::memcmp(d + 8, "ALIM3", 5) == 0;
}

bool has_vst_instruments(const uint8_t* d, size_t size) {
  size_t at = 0;
  while (at + 8 <= size) {
    const uint16_t tag = u16(d + at + 2);
    const uint32_t length = u32(d + at + 4);
    if (length > size - at - 8) return false;
    if (tag == 0xFFFF) return false;
    if (tag == 6) {
      // empty: nothing, or only the chunk that ends the row
      if (length < 8) return false;
      return u16(d + at + 8 + 2) != 0xFFFF;
    }
    at += 8 + length;
  }
  return false;
}

void text(char* to, size_t size, const char* key) {
  const char* s = openmpt_module_get_metadata(g_module, key);
  std::snprintf(to, size, "%s", s ? s : "");
  openmpt_free_string(s);
}

openmpt_module* load(const uint8_t* data, size_t size) {
  int error = 0;
  return openmpt_module_create_from_memory2(data, size, log_nothing, nullptr, nullptr, nullptr,
                                            &error, nullptr, nullptr);
}

void free_sends() {
  for (openmpt_module*& m : g_sends) {
    if (m) openmpt_module_destroy(m);
    m = nullptr;
  }
}

}  // namespace

extern "C" int synthw_skale_open(const char*, const uint8_t* data, size_t size, int track,
                                  synth_info* info) {
  if (!is_skm(data, size)) {
    std::snprintf(info->error, sizeof(info->error), "not a Skale Tracker song");
    return 0;
  }
  if (has_vst_instruments(data, size)) {
    std::snprintf(info->error, sizeof(info->error), "the song plays VST instruments, which can't be played here");
    return 0;
  }
  modland_skm_hook = 0;
  g_module = load(data, size);
  if (!g_module) {
    std::snprintf(info->error, sizeof(info->error), "the song can't be loaded");
    return 0;
  }
  char type[64];
  text(type, sizeof(type), "type");
  if (std::strcmp(type, "skm") != 0) {
    std::snprintf(info->error, sizeof(info->error), "not a Skale Tracker song");
    openmpt_module_destroy(g_module);
    g_module = nullptr;
    return 0;
  }
  const int tracks = openmpt_module_get_num_subsongs(g_module);
  if (track >= (tracks > 0 ? tracks : 1)) {
    std::snprintf(info->error, sizeof(info->error), "the song has %d tracks", tracks > 0 ? tracks : 1);
    return 0;
  }
  openmpt_module_select_subsong(g_module, track);
  openmpt_module_set_repeat_count(g_module, 0);
  free_sends();
  g_rate = 48000;
  if (skale::read_mixer(data, size, g_mixer)) {
    g_rate = 44100;
    for (int s = 0; s < skale::kSlots; ++s) {
      if (!g_mixer.fed(s)) continue;
      for (int i = 0; i < 256; ++i)
        modland_skm_gain[i] = static_cast<size_t>(i) < g_mixer.instrument_sends.size() ? g_mixer.instrument_sends[i][s] : 0.0f;
      modland_skm_hook = 1;
      g_sends[s] = load(data, size);
      modland_skm_hook = 0;
      if (!g_sends[s]) continue;
      openmpt_module_select_subsong(g_sends[s], track);
      openmpt_module_set_repeat_count(g_sends[s], 0);
    }
  }
  info->rate = g_rate;
  info->tracks = tracks > 0 ? tracks : 1;
  info->duration_ms = static_cast<long>(openmpt_module_get_duration_seconds(g_module) * 1000.0 + 0.5);
  text(info->title, sizeof(info->title), "title");
  text(info->author, sizeof(info->author), "artist");
  text(info->comment, sizeof(info->comment), "message_raw");
  text(info->type, sizeof(info->type), "type_long");
  if (!info->type[0]) std::snprintf(info->type, sizeof(info->type), "Skale Tracker");
  const int instruments = openmpt_module_get_num_instruments(g_module);
  for (int i = 0; i < instruments && info->n_names < SYNTH_MAX_NAMES; ++i) {
    const char* s = openmpt_module_get_instrument_name(g_module, i);
    std::snprintf(info->names[info->n_names++], sizeof(info->names[0]), "%s", s ? s : "");
    openmpt_free_string(s);
  }
  if (instruments == 0) {
    const int samples = openmpt_module_get_num_samples(g_module);
    for (int i = 0; i < samples && info->n_names < SYNTH_MAX_NAMES; ++i) {
      const char* s = openmpt_module_get_sample_name(g_module, i);
      std::snprintf(info->names[info->n_names++], sizeof(info->names[0]), "%s", s ? s : "");
      openmpt_free_string(s);
    }
  }
  g_ended = false;
  return 1;
}

extern "C" int synthw_skale_render(float* out, int frames) {
  if (!g_module || g_ended) return 0;
  int done = 0;
  while (done < frames) {
    const int want = frames - done;
    const int got = static_cast<int>(
        openmpt_module_read_interleaved_float_stereo(g_module, g_rate, want, out + done * 2));
    if (got == 0) { g_ended = true; break; }
    bool sending = false;
    for (int s = 0; s < skale::kSlots; ++s) {
      std::vector<float>& in = g_inputs[s];
      in.clear();
      if (!g_sends[s]) continue;
      in.assign(static_cast<size_t>(got) * 2, 0.0f);
      int have = 0;
      while (have < got) {
        const size_t n = openmpt_module_read_interleaved_float_stereo(g_sends[s], g_rate, got - have,
                                                                      in.data() + have * 2);
        if (n == 0) break;
        have += static_cast<int>(n);
      }
      sending = true;
    }
    if (sending) skale::run_buses(g_mixer, g_inputs, out + done * 2, got);
    done += got;
  }
  return done;
}

extern "C" void synthw_skale_close(void) {
  if (g_module) openmpt_module_destroy(g_module);
  g_module = nullptr;
  free_sends();
}
