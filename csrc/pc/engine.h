/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What every replayer ("engine") of bbmp-pc offers to the main program.
 *
 * Each engine is one source file (or a few) whose only visible names are its
 * pcw_<engine>_* functions; everything else in it is static or in an unnamed
 * namespace, so that engines can't collide. */
#ifndef BBMP_PC_ENGINE_H
#define BBMP_PC_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MAX_NAMES 512

typedef struct pc_info {
  int rate;             /* in: the rate asked for. out: the rate the engine renders at;
                           the main program resamples when they differ */
  int tracks;           /* subsongs, at least 1 */
  long duration_ms;     /* 0 when unknown */
  char title[256];
  char author[256];
  char type[64];
  char comment[8192];
  char error[256];      /* why open() failed */
  char names[PC_MAX_NAMES][68];
  int n_names;
} pc_info;

/* Text in pc_info is UTF-8, or else taken to be CP437 (DOS) by the main program.
 *
 * open():   1 when the file is this engine's format and is ready to play, else 0
 *           with info->error set. `data` stays valid until close().
 * render(): fills `out` with up to `frames` stereo frames of floats in -1..1 and
 *           returns how many it wrote; fewer than asked means the song has ended.
 * close():  frees what open() took. */
#define PC_ENGINE(name) \
  int pcw_##name##_open(const char* path, const uint8_t* data, size_t size, int track, \
                        pc_info* info); \
  int pcw_##name##_render(float* out, int frames); \
  void pcw_##name##_close(void);

PC_ENGINE(unitrk)
PC_ENGINE(playerpro)
PC_ENGINE(buzz)
PC_ENGINE(dsm)
PC_ENGINE(mxm)

/* For engines that convert a file to a module libopenmpt plays (convert.cpp):
 * hand over the converted module; render and close with pcw_openmpt_*. `type`
 * is what the header line shall say instead of the converted module's format. */
int pcw_openmpt_open(const uint8_t* module, size_t size, int track, const char* type,
                     pc_info* info);
int pcw_openmpt_render(float* out, int frames);
void pcw_openmpt_close(void);

#ifdef __cplusplus
}
#endif

#endif
