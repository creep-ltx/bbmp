/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SBStudio (.pac) through Thomas Pfaff's libpac 0.9.0, as rePlayer has it
 * (source/Replays/SBStudio/libpac: libpac with the names of the sounds kept). */

#include "engine.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacP.h"   /* includes pac.h; for the names of the sounds */

enum { kRate = 48000, kBlock = 4096 };

static struct pac_module* g_module;
static long g_block[kBlock * 2];

static void copy_name(char* to, size_t size, const char* from) {
  size_t n = 0;
  while (n + 1 < size && n < PAC_NAME_MAX && from[n]) { to[n] = from[n]; ++n; }
  while (n && (to[n - 1] == ' ' || to[n - 1] == '\t')) --n;
  to[n] = 0;
}

int synthw_sbstudio_open(const char* path, const uint8_t* data, size_t size, int track, synth_info* info) {
  int i;
  if (size < 16 || memcmp(data, "PACG", 4) != 0) {
    snprintf(info->error, sizeof(info->error), "not an SBStudio song");
    return 0;
  }
  if (track != 0) {
    snprintf(info->error, sizeof(info->error), "the song has one track");
    return 0;
  }
  /* 32 bits: the mix as it is, which the helper clips, and not libpac's 16 bits
   * with its own clipping. libpac lowers the volume to 60% for 16 bits, to make
   * room for the channels; that stays as it is for 16 bits. */
  if (pac_init(kRate, 32, 2) != 0) {
    snprintf(info->error, sizeof(info->error), "libpac can't be started");
    return 0;
  }
  pac_enable(PAC_VOLUME_REDUCTION);
  errno = 0;
  g_module = pac_open(path);
  if (!g_module) {
    snprintf(info->error, sizeof(info->error), "%s",
             errno == PAC_EFORMAT ? "the song is damaged or of a version libpac doesn't know"
                                  : "the song can't be loaded");
    pac_exit();
    return 0;
  }
  /* A song can be written for MIDI instruments alone and have no samples at all;
   * libpac plays the samples and would play nothing. */
  {
    int sounds = 0;
    for (i = 0; i < g_module->soundcnt; ++i)
      if (g_module->sound[i].sample && g_module->sound[i].length > 0) ++sounds;
    if (!sounds) {
      snprintf(info->error, sizeof(info->error), "the song has no samples (it is written for MIDI instruments)");
      pac_close(g_module);
      pac_exit();
      g_module = NULL;
      return 0;
    }
  }
  info->rate = kRate;
  info->tracks = 1;
  info->duration_ms = pac_length(g_module) > 0 ? (long)(pac_length(g_module) * 1000.0 / kRate) : 0;
  snprintf(info->type, sizeof(info->type), "SBStudio");
  copy_name(info->title, sizeof(info->title), g_module->name);
  for (i = 0; i < g_module->soundcnt && info->n_names < SYNTH_MAX_NAMES; ++i)
    copy_name(info->names[info->n_names++], sizeof(info->names[0]), g_module->sound[i].name);
  return 1;
}

int synthw_sbstudio_render(float* out, int frames) {
  int done = 0;
  if (!g_module) return 0;
  while (done < frames) {
    const int want = frames - done < kBlock ? frames - done : kBlock;
    const long got = pac_read(g_module, g_block, (long)want * 2 * (long)sizeof(long)) /
                     (2 * (long)sizeof(long));
    long i;
    if (got <= 0) break;
    for (i = 0; i < got * 2; ++i) out[done * 2 + i] = (float)g_block[i] / 32768.0f;
    done += (int)got;
  }
  return done;
}

void synthw_sbstudio_close(void) {
  if (g_module) {
    pac_close(g_module);
    pac_exit();
    g_module = NULL;
  }
}
