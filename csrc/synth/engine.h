/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What every replayer ("engine") of bbmp-synth offers to the main program.
 *
 * Each engine is built on its own and bundled into one object file whose only
 * visible names are its synthw_<engine>_* functions, so that the replayers'
 * global names can't collide. */
#ifndef BBMP_SYNTH_ENGINE_H
#define BBMP_SYNTH_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_MAX_NAMES 256

typedef struct synth_info {
  int rate;             /* the rate the engine renders at; the main program resamples */
  int tracks;           /* subsongs, at least 1 */
  long duration_ms;     /* 0 when unknown */
  char title[256];
  char author[256];
  char type[64];
  char comment[4096];
  char error[256];      /* why open() failed */
  char names[SYNTH_MAX_NAMES][68];
  int n_names;
} synth_info;

/* open():   1 when the file is this engine's format and is ready to play, else 0
 *           with info->error set.
 * render(): fills `out` with up to `frames` stereo frames of floats in -1..1 and
 *           returns how many it wrote; fewer than asked means the song has ended.
 * close():  frees what open() took. */
#define SYNTH_ENGINE(name) \
  int synthw_##name##_open(const char* path, const uint8_t* data, size_t size, int track, \
                           synth_info* info); \
  int synthw_##name##_render(float* out, int frames); \
  void synthw_##name##_close(void);

SYNTH_ENGINE(protrekkr)
SYNTH_ENGINE(kdm)
SYNTH_ENGINE(skale)
SYNTH_ENGINE(buz)
SYNTH_ENGINE(jxs)
SYNTH_ENGINE(sbstudio)

#ifdef __cplusplus
}
#endif

#endif
