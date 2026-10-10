// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pc: plays a module that an engine has converted to a format libopenmpt
// reads (XM, IT, S3M, MOD). The engines that convert call this instead of mixing
// themselves.

#include "engine.h"

#include <libopenmpt/libopenmpt.h>

#include <cstdio>
#include <cstring>

namespace {

openmpt_module* g_mod = nullptr;
int g_rate = 48000;

void copy(char* to, size_t size, const char* from) {
  if (!from) return;
  std::snprintf(to, size, "%s", from);
}

void log_nothing(const char*, void*) {}

}  // namespace

extern "C" int pcw_openmpt_open(const uint8_t* module, size_t size, int track, const char* type,
                                pc_info* info) {
  int error = 0;
  const char* message = nullptr;
  g_mod = openmpt_module_create_from_memory2(module, size, log_nothing, nullptr, nullptr, nullptr,
                                             &error, &message, nullptr);
  if (!g_mod) {
    std::snprintf(info->error, sizeof(info->error), "the converted module doesn't load: %s",
                  message ? message : "unknown error");
    if (message) openmpt_free_string(message);
    return 0;
  }
  g_rate = info->rate > 0 ? info->rate : 48000;
  info->rate = g_rate;
  info->tracks = openmpt_module_get_num_subsongs(g_mod);
  if (info->tracks < 1) info->tracks = 1;
  if (track >= info->tracks) {
    std::snprintf(info->error, sizeof(info->error), "no such track");
    openmpt_module_destroy(g_mod);
    g_mod = nullptr;
    return 0;
  }
  openmpt_module_select_subsong(g_mod, track);
  openmpt_module_set_repeat_count(g_mod, 0);
  info->duration_ms = static_cast<long>(openmpt_module_get_duration_seconds(g_mod) * 1000.0);
  if (!info->title[0]) {
    const char* t = openmpt_module_get_metadata(g_mod, "title");
    copy(info->title, sizeof(info->title), t);
    openmpt_free_string(t);
  }
  if (!info->comment[0]) {
    const char* t = openmpt_module_get_metadata(g_mod, "message_raw");
    copy(info->comment, sizeof(info->comment), t);
    openmpt_free_string(t);
  }
  copy(info->type, sizeof(info->type), type);
  if (info->n_names == 0) {
    const int ins = openmpt_module_get_num_instruments(g_mod);
    const int n = ins > 0 ? ins : openmpt_module_get_num_samples(g_mod);
    for (int i = 0; i < n && i < PC_MAX_NAMES; ++i) {
      const char* t = ins > 0 ? openmpt_module_get_instrument_name(g_mod, i)
                              : openmpt_module_get_sample_name(g_mod, i);
      copy(info->names[i], sizeof(info->names[i]), t);
      openmpt_free_string(t);
      info->n_names = i + 1;
    }
  }
  return 1;
}

extern "C" int pcw_openmpt_render(float* out, int frames) {
  if (!g_mod) return 0;
  int done = 0;
  while (done < frames) {
    const size_t got = openmpt_module_read_interleaved_float_stereo(
        g_mod, g_rate, static_cast<size_t>(frames - done), out + done * 2);
    if (got == 0) break;
    done += static_cast<int>(got);
  }
  return done;
}

extern "C" void pcw_openmpt_close(void) {
  if (g_mod) openmpt_module_destroy(g_mod);
  g_mod = nullptr;
}
